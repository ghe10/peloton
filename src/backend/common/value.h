//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// value.h
//
// Identification: src/backend/common/value.h
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <stdint.h>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <iostream>

#include "backend/common/exception.h"
#include "backend/common/pool.h"
#include "backend/common/serializer.h"
#include "backend/common/types.h"
#include "backend/common/varlen.h"
#include "backend/common/logger.h"

#include "boost/scoped_ptr.hpp"
#include "boost/functional/hash.hpp"
#include "ttmath/ttmathint.h"
#include "utf8.h"
#include "murmur3/MurmurHash3.h"

namespace peloton {

//===--------------------------------------------------------------------===//
// Type system
//===--------------------------------------------------------------------===//

/*
 * Objects are length preceded with a short length value or a long length value
 * depending on how many bytes are needed to represent the length. These
 * define how many bytes are used for the short value vs. the long value.
 */
#define SHORT_OBJECT_LENGTHLENGTH static_cast<char>(1)
#define LONG_OBJECT_LENGTHLENGTH static_cast<char>(4)
#define OBJECT_NULL_BIT static_cast<char>(1 << 6)
#define OBJECT_CONTINUATION_BIT static_cast<char>(1 << 7)
#define OBJECT_MAX_LENGTH_SHORT_LENGTH 63

#define FULL_STRING_IN_MESSAGE_THRESHOLD 100

//The int used for storage and return values
typedef ttmath::Int<2> TTInt;
//Long integer with space for multiplication and division without carry/overflow
typedef ttmath::Int<4> TTLInt;

template<typename T>
void throwCastSQLValueOutOfRangeException(
    const T value,
    const ValueType origType,
    const ValueType newType)
{
  throwCastSQLValueOutOfRangeException((const int64_t)value, origType, newType);
}

template<>
inline void throwCastSQLValueOutOfRangeException<double>(
    const double value,
    const ValueType origType,
    const ValueType newType)
    {
  char msg[1024];
  snprintf(msg, 1024, "Type %s with value %f can't be cast as %s because the value is "
           "out of range for the destination type",
           ValueTypeToString(origType).c_str(),
           value,
           ValueTypeToString(newType).c_str());

  throw NumericValueOutOfRangeException(msg, 0);
    }

template<>
inline void throwCastSQLValueOutOfRangeException<int64_t>(
    const int64_t value,
    const ValueType origType,
    const ValueType newType)
    {
  char msg[1024];
  snprintf(msg, 1024, "Type %s with value %jd can't be cast as %s because the value is "
           "out of range for the destination type",
           ValueTypeToString(origType).c_str(),
           (intmax_t)value,
           ValueTypeToString(newType).c_str());

  // record underflow or overflow for executors that catch this (indexes, mostly)
  int internalFlags = 0;
  if (value > 0) internalFlags |= NumericValueOutOfRangeException::TYPE_OVERFLOW;
  if (value < 0) internalFlags |= NumericValueOutOfRangeException::TYPE_UNDERFLOW;

  throw NumericValueOutOfRangeException(msg, internalFlags);
    }

template<>
inline void throwCastSQLValueOutOfRangeException<TTInt>(
    const TTInt value,
    const ValueType origType,
    const ValueType newType)
    {
  char msg[1024];
  snprintf(msg, 1024, "Type %s with value %s can't be cast as %s because the value is "
           "out of range for the destination type",
           ValueTypeToString(origType).c_str(),
           value.ToString().c_str(),
           ValueTypeToString(newType).c_str());

  // record underflow or overflow for executors that catch this (indexes, mostly)
  int internalFlags = 0;
  if (value > 0) internalFlags |= NumericValueOutOfRangeException::TYPE_OVERFLOW;
  if (value < 0) internalFlags |= NumericValueOutOfRangeException::TYPE_UNDERFLOW;

  throw NumericValueOutOfRangeException(msg, internalFlags);
    }

int warn_if(int condition, const char* message);

// This has been demonstrated to be more reliable than std::isinf
// -- less sensitive on LINUX to the "g++ -ffast-math" option.
inline int non_std_isinf( double x ) { return (x > DBL_MAX) || (x < -DBL_MAX); }

inline void throwDataExceptionIfInfiniteOrNaN(double value, const char* function)
{
  static int warned_once_no_nan = warn_if( ! std::isnan(sqrt(-1.0)),
                                           "The C++ configuration (e.g. \"g++ --fast-math\") "
                                           "does not support SQL standard handling of NaN errors.");
  static int warned_once_no_inf = warn_if( ! non_std_isinf(std::pow(0.0, -1.0)),
                                           "The C++ configuration (e.g. \"g++ --fast-math\") "
                                           "does not support SQL standard handling of numeric infinity errors.");
  // This uses a standard test for NaN, even though that fails in some configurations like LINUX "g++ -ffast-math".
  // If it is known to fail in the current config, a warning has been sent to the log,
  // so at this point, just relax the check.
  if ((warned_once_no_nan || ! std::isnan(value)) && (warned_once_no_inf || ! non_std_isinf(value))) {
    return;
  }
  char msg[1024];
  snprintf(msg, sizeof(msg), "Invalid result value (%f) from floating point %s", value, function);
  throw NumericValueOutOfRangeException(std::string(msg), 0);
}


/// Stream out a double value in SQL standard format, a specific variation of E-notation.
/// TODO: it has been suggested that helper routines like this that are not specifically tied to
/// the Value representation should be defined in some other header to help reduce clutter, here.
inline void streamSQLFloatFormat(std::stringstream& streamOut, double floatValue)
{
  // Standard SQL wants capital E scientific notation.
  // Yet it differs in some detail from C/C++ E notation, even with all of its customization options.

  // For starters, for 0, the standard explicitly calls for '0E0'.
  // For across-the-board compatibility, the HSQL backend had to be patched it was using '0.0E0'.
  // C++ uses 0.000000E+00 by default. So override that explicitly.
  if (0.0 == floatValue) {
    streamOut << "0E0";
    return;
  }
  // For other values, C++ generally adds too much garnish to be standard
  // -- trailing zeros in the mantissa, an explicit '+' on the exponent, and a
  // leading 0 before single-digit exponents.  Trim it down to the minimalist sql standard.
  std::stringstream fancy;
  fancy << std::setiosflags(std::ios::scientific | std::ios::uppercase) << floatValue;
  // match any format with the regular expression:
  std::string fancyText = fancy.str();
  size_t ePos = fancyText.find('E', 3); // find E after "[-]n.n".
  assert(ePos != std::string::npos);
  size_t endSignifMantissa;
  // Never truncate mantissa down to the bare '.' EVEN for the case of "n.0".
  for (endSignifMantissa = ePos; fancyText[endSignifMantissa-2] != '.'; --endSignifMantissa) {
    // Only truncate trailing '0's.
    if (fancyText[endSignifMantissa-1] != '0') {
      break; // from loop
    }
  }
  const char* optionalSign = (fancyText[ePos+1] == '-') ? "-" : "";
  size_t startSignifExponent;
  // Always keep at least 1 exponent digit.
  size_t endExponent = fancyText.length()-1;
  for (startSignifExponent = ePos+1; startSignifExponent < endExponent; ++startSignifExponent) {
    const char& exponentLeadChar = fancyText[startSignifExponent];
    // Only skip leading '-'s, '+'s and '0's.
    if (exponentLeadChar != '-' && exponentLeadChar != '+' && exponentLeadChar != '0') {
      break; // from loop
    }
  }
  // Bring the truncated pieces together.
  streamOut << fancyText.substr(0, endSignifMantissa)
                                      << 'E' << optionalSign << fancyText.substr(startSignifExponent);
}

/**
 * A class to wrap all scalar values regardless of type and
 * storage. An Value is not the representation used in the
 * serialization of VoltTables nor is it the representation of how
 * scalar values are stored in tables. Value does have serialization
 * and deserialization mechanisms for both those storage formats.
 * Values are designed to be immutable and for the most part not
 * constructable from raw data types. Access to the raw data is
 * restricted so that all operations have to go through the member
 * functions that can perform the correct casting and error
 * checking. ValueFactory can be used to construct new Values, but
 * that should be avoided if possible.
 */
class Value {
  friend class ValuePeeker;
  friend class ValueFactory;

 public:
  /* Create a default Value */
  Value();

  // todo: free() should not really be const

  /* Release memory associated to object type Values */
  void free() const;

  /* Release memory associated to object type tuple columns */
  static void freeObjectsFromTupleStorage(std::vector<char*> const &oldObjects);

  /* Set value to the correct SQL NULL representation. */
  void setNull();

  /* Reveal the contained pointer for type values  */
  void* castAsAddress() const;

  /* Create a boolean true Value */
  static Value getTrue();

  /* Create a boolean false Value */
  static Value getFalse();

  /* Create an Value with the null representation for valueType */
  static Value getNullValue(ValueType);

  /* Create an Value promoted/demoted to type */
  Value castAs(ValueType type) const;

  // todo: Why doesn't this return size_t? Also, this is a
  // quality of ValueType, not Value.

  /* Calculate the tuple storage size for an Value type. VARCHARs
       assume out-of-band tuple storage */
  static uint16_t getTupleStorageSize(const ValueType type);

  // todo: Could the isInlined argument be removed by have the
  // caller dereference the pointer?

  /* Deserialize a scalar of the specified type from the tuple
       storage area provided. If this is an Object type then the third
       argument indicates whether the object is stored in the tuple
       inline */
  static Value initFromTupleStorage(const void *storage, ValueType type, bool isInlined);

  /* Serialize the scalar this Value represents to the provided
       storage area. If the scalar is an Object type that is not
       inlined then the provided data pool or the heap will be used to
       allocated storage for a copy of the object. */
  void serializeToTupleStorageAllocateForObjects(
      void *storage, const bool isInlined, const int32_t maxLength,
      const bool isInBytes, VarlenPool *dataPool) const;

  /* Serialize the scalar this Value represents to the storage area
       provided. If the scalar is an Object type then the object will
       be copy if it can be inlined into the tuple. Otherwise a
       pointer to the object will be copied into the storage area. Any
       allocations needed (if this Value refers to inlined memory
       whereas the field in the tuple is not inlined), will be done in
       the temp string pool. */
  void serializeToTupleStorage(
      void *storage, const bool isInlined, const int32_t maxLength, const bool isInBytes) const;

  /* Deserialize a scalar value of the specified type from the
       SerializeInput directly into the tuple storage area
       provided. This function will perform memory allocations for
       Object types as necessary using the provided data pool or the
       heap. This is used to deserialize tables. */
  template <TupleSerializationFormat F, Endianess E>
  static void deserializeFrom(
      SerializeInput<E> &input, VarlenPool *dataPool, char *storage,
      const ValueType type, bool isInlined, int32_t maxLength, bool isInBytes);
  static void deserializeFrom(
      SerializeInputBE &input, VarlenPool *dataVarlenPool, char *storage,
      const ValueType type, bool isInlined, int32_t maxLength, bool isInBytes);

  // TODO: no callers use the first form; Should combine these
  // eliminate the potential Value copy.

  /* Read a ValueType from the SerializeInput stream and deserialize
       a scalar value of the specified type into this Value from the provided
       SerializeInput and perform allocations as necessary. */
  void deserializeFromAllocateForStorage(SerializeInputBE &input, VarlenPool *dataPool);
  void deserializeFromAllocateForStorage(ValueType vt, SerializeInputBE &input, VarlenPool *dataPool);

  /* Serialize this Value to a SerializeOutput */
  void serializeTo(SerializeOutput &output) const;

  /* Serialize this Value to an Export stream */
  void serializeToExport_withoutNull(ExportSerializeOutput&) const;

  // See comment with inlined body, below.  If NULL is supplied for
  // the pool, use the temp string pool.
  void allocateObjectFromInlinedValue(VarlenPool* pool);

  void allocateObjectFromOutlinedValue();

  /* Check if the value represents SQL NULL */
  bool isNull() const;

  /* Check if the value represents IEEE 754 NaN */
  bool isNaN() const;

  /* For boolean Values, convert to bool */
  bool isTrue() const;
  bool isFalse() const;

  /* Tell caller if this Value's value refers back to VARCHAR or
       VARBINARY data internal to a TableTuple (and not a
       Varlen) */
  bool getSourceInlined() const;

  /* For number values, check the number line. */
  bool isZero() const;

  /* For boolean Values only, logical operators */
  Value op_negate() const;
  Value op_and(const Value rhs) const;
  Value op_or(const Value rhs) const;

  /* Evaluate the ordering relation against two Values. Promotes
       exact types to allow disparate type comparison. See also the
       op_ functions which return boolean Values.
   */
  int compareNull(const Value rhs) const;
  int compare(const Value rhs) const;
  int compare_withoutNull(const Value rhs) const;

  /* Return a boolean Value with the comparison result */
  Value op_equals(const Value rhs) const;
  Value op_notEquals(const Value rhs) const;
  Value op_lessThan(const Value rhs) const;
  Value op_lessThanOrEqual(const Value rhs) const;
  Value op_greaterThan(const Value rhs) const;
  Value op_greaterThanOrEqual(const Value rhs) const;

  Value op_equals_withoutNull(const Value rhs) const;
  Value op_notEquals_withoutNull(const Value rhs) const;
  Value op_lessThan_withoutNull(const Value rhs) const;
  Value op_lessThanOrEqual_withoutNull(const Value rhs) const;
  Value op_greaterThan_withoutNull(const Value rhs) const;
  Value op_greaterThanOrEqual_withoutNull(const Value rhs) const;


  /* Return a copy of MAX(this, rhs) */
  Value op_max(const Value rhs) const;

  /* Return a copy of MIN(this, rhs) */
  Value op_min(const Value rhs) const;

  /* For number Values, compute new Values for arithmetic operators */
  Value op_increment() const;
  Value op_decrement() const;
  Value op_subtract(const Value rhs) const;
  Value op_add(const Value rhs) const;
  Value op_multiply(const Value rhs) const;
  Value op_divide(const Value rhs) const;
  /*
   * This Value must be VARCHAR and the rhs must be VARCHAR.
   * This Value is the value and the rhs is the pattern
   */
  Value like(const Value rhs) const;

  //TODO: passing Value arguments by const reference SHOULD be standard practice
  // for the dozens of Value "operator" functions. It saves on needless Value copies.
  //TODO: returning bool (vs. Value getTrue()/getFalse()) SHOULD be standard practice
  // for Value "logical operator" functions.
  // It saves on needless Value copies and makes unit tests more readable.
  // Cases that need the Value -- for some actual purpose other than an immediate call to
  // "isTrue()" -- are rare and getting rarer as optimizations like short-cut eval are introduced.
  /**
   * Return true if this Value is listed as a member of the IN LIST
   * represented as an ValueList* value cached in rhsList.
   */
  bool inList(Value const& rhsList) const;

  /**
   * If this Value is an array value, get it's length.
   * Undefined behavior if not an array (cassert fail in debug).
   */
  int arrayLength() const;

  /**
   * If this Value is an array value, get a value.
   * Undefined behavior if not an array or if oob (cassert fail in debug).
   */
  Value itemAtIndex(int index) const;

  /**
   * Used for SQL-IN-LIST to cast all array values to a specific type,
   * then sort an dedup them. Returns in a parameter vector, mostly for memory
   * management reasons. Dedup is important for index-accelerated plans, as
   * they might return duplicate rows from the inner join.
   * See MaterializedScanPlanNode & MaterializedScanExecutor
   *
   * Undefined behavior if not an array (cassert fail in debug).
   */
  void castAndSortAndDedupArrayForInList(const ValueType outputType, std::vector<Value> &outList) const;

  /*
   * Out must have space for 16 bytes
   */
  int32_t murmurHash3() const;

  /*
   * callConstant, callUnary, and call are templates for arbitrary Value member functions that implement
   * SQL "column functions". They differ in how many arguments they accept:
   * 0 for callConstant, 1 ("this") for callUnary, and any number (packaged in a vector) for call.
   * The main benefit of these functions being (always explicit) template instantiations for each
   * "FUNC_*" int value instead of a more normal named member function
   * of Value is that it allows them to be invoked from the default eval method of the
   * correspondingly templated expression subclass
   * (ConstantFunctionExpression, UnaryFunctionExpression, or GeneralFunctionExpression).
   * The alternative would be to name each function (abs, substring, etc.)
   * and explicitly implement the eval method for every expression subclass template instantiation
   * (UnaryFunctionExpression<FUNC_ABS>::eval,
   * GeneralFunctionExpression<FUNC_LOG_SUBSTRING_FROM>::eval, etc.
   * to call the corresponding Value member function.
   * So, these member function templates save a bit of boilerplate for each SQL function and allow
   * the function expression subclass templates to be implemented completely generically.
   */
  template <int F> // template for SQL functions returning constants (like pi)
  static Value callConstant();

  template <int F> // template for SQL functions of one Value ("this")
  Value callUnary() const;

  template <int F> // template for SQL functions of multiple Values
  static Value call(const std::vector<Value>& arguments);

  /// Iterates over UTF8 strings one character "code point" at a time, being careful not to walk off the end.
  class UTF8Iterator {
   public:
    UTF8Iterator(const char *start, const char *end) :
      m_cursor(start),
      m_end(end)
   // TODO: We could validate up front that the string is well-formed UTF8,
   // at least to the extent that multi-byte characters have a valid
   // prefix byte and continuation bytes that will not cause a read
   // off the end of the buffer.
   // That done, extractCodePoint could be considerably simpler/faster.
   { assert(m_cursor <= m_end); }

    //Construct a one-off with an alternative current cursor position
    UTF8Iterator(const UTF8Iterator& other, const char *start) :
      m_cursor(start),
      m_end(other.m_end)
    { assert(m_cursor <= m_end); }

    const char * getCursor() { return m_cursor; }

    bool atEnd() { return m_cursor >= m_end; }

    const char * skipCodePoints(int64_t skips) {
      while (skips-- > 0 && ! atEnd()) {
        // TODO: since the returned code point is ignored, it might be better
        // to call a faster, simpler, skipCodePoint method -- maybe once that
        // becomes trivial due to up-front validation.
        extractCodePoint();
      }
      if (atEnd()) {
        return m_end;
      }
      return m_cursor;
    }

    /*
     * Go through a lot of trouble to make sure that corrupt
     * utf8 data doesn't result in touching uninitialized memory
     * by copying the character data onto the stack.
     * That wouldn't be needed if we pre-validated the buffer.
     */
    uint32_t extractCodePoint() {
      assert(m_cursor < m_end); // Caller should have tested and handled atEnd() condition
      /*
       * Copy the next 6 bytes to a temp buffer and retrieve.
       * We should only get 4 byte code points, and the library
       * should only accept 4 byte code points, but once upon a time there
       * were 6 byte code points in UTF-8 so be careful here.
       */
      char nextPotentialCodePoint[] = { 0, 0, 0, 0, 0, 0 };
      char *nextPotentialCodePointIter = nextPotentialCodePoint;
      //Copy 6 bytes or until the end
      ::memcpy( nextPotentialCodePoint, m_cursor, std::min( 6L, m_end - m_cursor));

      /*
       * Extract the code point, find out how many bytes it was
       */
      uint32_t codePoint = utf8::unchecked::next(nextPotentialCodePointIter);
      long int delta = nextPotentialCodePointIter - nextPotentialCodePoint;

      /*
       * Increment the iterator that was passed in by ref, by the delta
       */
      m_cursor += delta;
      return codePoint;
    }

    const char * m_cursor;
    const char * const m_end;
  };


  /* For boost hashing */
  void hashCombine(std::size_t &seed) const;

  /* Functor comparator for use with std::set */
  struct ltValue {
    bool operator()(const Value v1, const Value v2) const {
      return v1.compare(v2) < 0;
    }
  };

  /* Functor equality predicate for use with boost unordered */
  struct equal_to : std::binary_function<Value, Value, bool>
  {
    bool operator()(Value const& x,
                    Value const& y) const
    {
      return x.compare(y) == 0;
    }
  };

  /* Functor hash predicate for use with boost unordered */
  struct hash : std::unary_function<Value, std::size_t>
  {
    std::size_t operator()(Value const& x) const
    {
      std::size_t seed = 0;
      x.hashCombine(seed);
      return seed;
    }
  };

  /* Return a string full of arcana and wonder. */
  std::string debug() const;

  // Constants for Decimal type
  // Precision and scale (inherent in the schema)
  static const uint16_t kMaxDecPrec = 38;
  static const uint16_t kMaxDecScale = 12;
  static const int64_t kMaxScaleFactor = 1000000000000;         // == 10**12
 private:
  // Our maximum scale is 12.  Our maximum precision is 38.  So,
  // the maximum number of decimal digits is 38 - 12 = 26.  We can't
  // represent 10**26 in a 64 bit integer, but we can represent 10**18.
  // So, to test if a TTInt named m is too big we test if
  // m/kMaxWholeDivisor < kMaxWholeFactor
  static const uint64_t kMaxWholeDivisor = 100000000;             // == 10**8
  static const uint64_t kMaxWholeFactor = 1000000000000000000;    // == 10**18
  static bool inline oversizeWholeDecimal(TTInt ii) {
    return (TTInt(kMaxWholeFactor) <= ii / kMaxWholeDivisor);
  }
 public:
  // setArrayElements is a const method since it doesn't actually mutate any Value state, just
  // the state of the contained Values which are referenced via the allocated object storage.
  // For example, it is not intended to ever "grow the array" which would require the Value's
  // object reference (in m_data) to be mutable.
  // The array size is predetermined in allocateANewValueList.
  void setArrayElements(std::vector<Value> &args) const;

  static ValueType promoteForOp(ValueType vta, ValueType vtb) {
    ValueType rt;
    switch (vta) {
      case VALUE_TYPE_TINYINT:
      case VALUE_TYPE_SMALLINT:
      case VALUE_TYPE_INTEGER:
      case VALUE_TYPE_BIGINT:
      case VALUE_TYPE_TIMESTAMP:
        rt = s_intPromotionTable[vtb];
        break;

      case VALUE_TYPE_DECIMAL:
        rt = s_decimalPromotionTable[vtb];
        break;

      case VALUE_TYPE_DOUBLE:
        rt = s_doublePromotionTable[vtb];
        break;

        // no valid promotion (currently) for these types
      case VALUE_TYPE_ADDRESS:
      case VALUE_TYPE_VARCHAR:
      case VALUE_TYPE_VARBINARY:
      case VALUE_TYPE_BOOLEAN:
      case VALUE_TYPE_INVALID:
      case VALUE_TYPE_NULL:
      default:
        rt = VALUE_TYPE_INVALID;
        break;
    }
    // There ARE rare but legitimate runtime type check exceptions in SQL, so
    // unless/until those legitimate cases get re-routed to some other code path,
    // it is not safe here to ...
    // assert(rt != VALUE_TYPE_INVALID);
    return rt;
  }

  // Declared public for cppunit test purposes .
  static int64_t parseTimestampString(const std::string &txt);

  static inline int32_t getCharLength(const char *valueChars, const size_t length) {
    // very efficient code to count characters in UTF string and ASCII string
    int32_t j = 0;
    size_t i = length;
    while (i-- > 0) {
      if ((valueChars[i] & 0xc0) != 0x80) j++;
    }
    return j;
  }

  static inline int32_t getIthCharIndex(const char *valueChars, const int64_t length, const int64_t ith) {
    if (ith <= 0) return -1;
    int32_t i = 0, j = 0;

    while (i < length) {
      if ((valueChars[i] & 0xc0) != 0x80) {
        if (++j == ith) break;
      }
      i++;
    }
    return i;
  }

  // Return the beginning char * place of the ith char.
  // Return the end char* when ith is larger than it has, NULL if ith is less and equal to zero.
  static inline const char* getIthCharPosition(const char *valueChars, const size_t length, const int32_t ith) {
    // very efficient code to count characters in UTF string and ASCII string
    int32_t i = getIthCharIndex(valueChars,length, ith);
    if (i < 0) return NULL;
    return &valueChars[i];
  }

  // Copy a value. If the value is inlined in a source tuple, then allocate
  // memory from the temp string pool and copy data there
  Value copyValue() const
  {
    Value copy = *this;
    if (m_sourceInlined) {
      // The Value storage is inlined (a pointer to the backing tuple storage) and needs
      // to be copied to a local storage
      copy.allocateObjectFromInlinedValue(getTempStringPool());
    }
    return copy;
  }

 private:
  /*
   * Private methods are private for a reason. Don't expose the raw
   * data so that it can be operated on directly.
   */

  // Function declarations for Value.cpp definitions.
  void createDecimalFromString(const std::string &txt);
  std::string createStringFromDecimal() const;

  // Helpers for inList.
  // These are purposely not inlines to avoid exposure of ValueList details.
  void deserializeIntoANewValueList(SerializeInputBE &input, VarlenPool *dataPool);
  void allocateANewValueList(size_t elementCount, ValueType elementType);

  // Promotion Rules. Initialized in Value.cpp
  static ValueType s_intPromotionTable[];
  static ValueType s_decimalPromotionTable[];
  static ValueType s_doublePromotionTable[];
  static TTInt s_maxDecimalValue;
  static TTInt s_minDecimalValue;
  // These initializers give the unique double values that are
  // closest but not equal to +/-1E26 within the accuracy of a double.
  static const double s_gtMaxDecimalAsDouble;
  static const double s_ltMinDecimalAsDouble;
  // These are the bound of converting decimal
  static TTInt s_maxInt64AsDecimal;
  static TTInt s_minInt64AsDecimal;

  /**
   * 16 bytes of storage for Value data.
   */
  char m_data[16];
  ValueType m_valueType;
  bool m_sourceInlined;

  /**
   * Private constructor that initializes storage and the specifies the type of value
   * that will be stored in this instance
   */
  Value(const ValueType type) {
    ::memset( m_data, 0, 16);
    setValueType(type);
    m_sourceInlined = false;
  }

  /**
   * Set the type of the value that will be stored in this instance.
   * The last of the 16 bytes of storage allocated in an Value
   * is used to store the type
   */
  void setValueType(ValueType type) {
    m_valueType = type;
  }

  /**
   * Get the type of the value. This information is private
   * to prevent code outside of Value from branching based on the type of a value.
   */
  ValueType getValueType() const {
    return m_valueType;
  }

  /**
   * Get the type of the value. This information is private
   * to prevent code outside of Value from branching based on the type of a value.
   */
  std::string getValueTypeString() const {
    return ValueTypeToString(m_valueType);
  }

  void setSourceInlined(bool sourceInlined)
  {
    m_sourceInlined = sourceInlined;
  }

  void tagAsNull() { m_data[13] = OBJECT_NULL_BIT; }

  /**
   * An Object is something like a String that has a variable length
   * (thus it is length preceded) and can potentially have indirect
   * storage (will always be indirect when referenced via an Value).
   * Values cache a decoded version of the length preceding value
   * in their data area after the pointer to the object storage area.
   *
   * Leverage private access and enforce strict requirements on
   * calling correctness.
   */
  int32_t getObjectLength_withoutNull() const {
    assert(isNull() == false);
    assert(getValueType() == VALUE_TYPE_VARCHAR || getValueType() == VALUE_TYPE_VARBINARY);
    // now safe to read and return the length preceding value.
    return *reinterpret_cast<const int32_t *>(&m_data[8]);
  }


  int8_t setObjectLength(int32_t length) {
    *reinterpret_cast<int32_t *>(&m_data[8]) = length;
    int8_t lengthLength = getAppropriateObjectLengthLength(length);
    setObjectLengthLength(lengthLength);
    return lengthLength;
  }

  /*
   * Retrieve the number of bytes used by the length preceding value
   * in the object's storage area. This value
   * is cached in the Value's 13th byte.
   */
  int8_t getObjectLengthLength() const {
    return m_data[12];
  }

  /*
   * Set the objects length preceding values length to
   * the specified value
   */
  void setObjectLengthLength(int8_t length) {
    m_data[12] = length;
  }

  /*
   * Based on the objects actual length value get the length of the
   * length preceding value to the appropriate length
   */
  static int8_t getAppropriateObjectLengthLength(int32_t length) {
    if (length <= OBJECT_MAX_LENGTH_SHORT_LENGTH) {
      return SHORT_OBJECT_LENGTHLENGTH;
    } else {
      return LONG_OBJECT_LENGTHLENGTH;
    }
  }

  /*
   * Set the length preceding value using the short or long representation depending
   * on what is necessary to represent the length.
   */
  static void setObjectLengthToLocation(int32_t length, char *location) {
    int32_t beNumber = htonl(length);
    if (length < -1) {
      throw Exception("Object length cannot be < -1");
    } else if (length == -1) {
      location[0] = OBJECT_NULL_BIT;
    } if (length <= OBJECT_MAX_LENGTH_SHORT_LENGTH) {
      location[0] = reinterpret_cast<char*>(&beNumber)[3];
    } else {
      char *pointer = reinterpret_cast<char*>(&beNumber);
      location[0] = pointer[0];
      location[0] |= OBJECT_CONTINUATION_BIT;
      location[1] = pointer[1];
      location[2] = pointer[2];
      location[3] = pointer[3];
    }
  }

  /*
   * Not truly symmetrical with getObjectValue which returns the actual object past
   * the length preceding value
   */
  void setObjectValue(void* object) {
    *reinterpret_cast<void**>(m_data) = object;
  }

  void* getObjectValue_withoutNull() const {
    void* value;
    if (m_sourceInlined) {
      value = *reinterpret_cast<char* const*>(m_data) + getObjectLengthLength();
    }
    else {
      Varlen* sref = *reinterpret_cast<Varlen* const*>(m_data);
      value = sref->Get() + getObjectLengthLength();
    }
    return value;
  }

  /**
   * Get a pointer to the value of an Object that lies beyond the storage of the length information
   */
  void* getObjectValue() const {
    if (isNull()) {
      return NULL;
    }
    return getObjectValue_withoutNull();
  }

  // getters
  const int8_t& getTinyInt() const {
    assert(getValueType() == VALUE_TYPE_TINYINT);
    return *reinterpret_cast<const int8_t*>(m_data);
  }

  int8_t& getTinyInt() {
    assert(getValueType() == VALUE_TYPE_TINYINT);
    return *reinterpret_cast<int8_t*>(m_data);
  }

  const int16_t& getSmallInt() const {
    assert(getValueType() == VALUE_TYPE_SMALLINT);
    return *reinterpret_cast<const int16_t*>(m_data);
  }

  int16_t& getSmallInt() {
    assert(getValueType() == VALUE_TYPE_SMALLINT);
    return *reinterpret_cast<int16_t*>(m_data);
  }

  const int32_t& getInteger() const {
    assert(getValueType() == VALUE_TYPE_INTEGER);
    return *reinterpret_cast<const int32_t*>(m_data);
  }

  int32_t& getInteger() {
    assert(getValueType() == VALUE_TYPE_INTEGER);
    return *reinterpret_cast<int32_t*>(m_data);
  }

  const int64_t& getBigInt() const {
    assert((getValueType() == VALUE_TYPE_BIGINT) ||
           (getValueType() == VALUE_TYPE_TIMESTAMP) ||
           (getValueType() == VALUE_TYPE_ADDRESS));
    return *reinterpret_cast<const int64_t*>(m_data);
  }

  int64_t& getBigInt() {
    assert((getValueType() == VALUE_TYPE_BIGINT) ||
           (getValueType() == VALUE_TYPE_TIMESTAMP) ||
           (getValueType() == VALUE_TYPE_ADDRESS));
    return *reinterpret_cast<int64_t*>(m_data);
  }

  const int64_t& getTimestamp() const {
    assert(getValueType() == VALUE_TYPE_TIMESTAMP);
    return *reinterpret_cast<const int64_t*>(m_data);
  }

  int64_t& getTimestamp() {
    assert(getValueType() == VALUE_TYPE_TIMESTAMP);
    return *reinterpret_cast<int64_t*>(m_data);
  }

  const double& getDouble() const {
    assert(getValueType() == VALUE_TYPE_DOUBLE);
    return *reinterpret_cast<const double*>(m_data);
  }

  double& getDouble() {
    assert(getValueType() == VALUE_TYPE_DOUBLE);
    return *reinterpret_cast<double*>(m_data);
  }

  const TTInt& getDecimal() const {
    assert(getValueType() == VALUE_TYPE_DECIMAL);
    const void* retval = reinterpret_cast<const void*>(m_data);
    return *reinterpret_cast<const TTInt*>(retval);
  }

  TTInt& getDecimal() {
    assert(getValueType() == VALUE_TYPE_DECIMAL);
    void* retval = reinterpret_cast<void*>(m_data);
    return *reinterpret_cast<TTInt*>(retval);
  }

  const bool& getBoolean() const {
    assert(getValueType() == VALUE_TYPE_BOOLEAN);
    return *reinterpret_cast<const bool*>(m_data);
  }

  bool& getBoolean() {
    assert(getValueType() == VALUE_TYPE_BOOLEAN);
    return *reinterpret_cast<bool*>(m_data);
  }

  bool isBooleanNULL() const ;

  std::size_t getAllocationSizeForObject() const;
  static std::size_t getAllocationSizeForObject(int32_t length);

  static void throwCastSQLException(const ValueType origType,
                                    const ValueType newType)
  {
    char msg[1024];
    snprintf(msg, 1024, "Type %s can't be cast as %s",
             ValueTypeToString(origType).c_str(),
             ValueTypeToString(newType).c_str());
    throw TypeMismatchException(msg, origType, newType);
  }

  /** return the whole part of a TTInt*/
  static inline int64_t narrowDecimalToBigInt(TTInt &scaledValue) {
    if (scaledValue > Value::s_maxInt64AsDecimal || scaledValue < Value::s_minInt64AsDecimal) {
      throwCastSQLValueOutOfRangeException<TTInt>(scaledValue, VALUE_TYPE_DECIMAL, VALUE_TYPE_BIGINT);
    }
    TTInt whole(scaledValue);
    whole /= kMaxScaleFactor;
    return whole.ToInt();
  }

  /** return the fractional part of a TTInt*/
  static inline int64_t getFractionalPart(TTInt& scaledValue) {
    TTInt fractional(scaledValue);
    fractional %= kMaxScaleFactor;
    return fractional.ToInt();
  }

  /**
   * Implicitly converting function to big integer type
   * DOUBLE, DECIMAL should not be handled here
   */
  int64_t castAsBigIntAndGetValue() const {
    assert(isNull() == false);

    const ValueType type = getValueType();
    assert(type != VALUE_TYPE_NULL);
    switch (type) {
      case VALUE_TYPE_TINYINT:
        return static_cast<int64_t>(getTinyInt());
      case VALUE_TYPE_SMALLINT:
        return static_cast<int64_t>(getSmallInt());
      case VALUE_TYPE_INTEGER:
        return static_cast<int64_t>(getInteger());
      case VALUE_TYPE_BIGINT:
        return getBigInt();
      case VALUE_TYPE_TIMESTAMP:
        return getTimestamp();
      default:
        throwCastSQLException(type, VALUE_TYPE_BIGINT);
        return 0; // NOT REACHED
    }
  }

  /**
   * Implicitly converting function to integer type
   * DOUBLE, DECIMAL should not be handled here
   */
  int32_t castAsIntegerAndGetValue() const {
    assert(isNull() == false);

    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_NULL:
        return INT32_NULL;
      case VALUE_TYPE_TINYINT:
        return static_cast<int32_t>(getTinyInt());
      case VALUE_TYPE_SMALLINT:
        return static_cast<int32_t>(getSmallInt());
      case VALUE_TYPE_INTEGER:
        return getInteger();
      case VALUE_TYPE_BIGINT:
      {
        const int64_t value = getBigInt();
        if (value > (int64_t)INT32_MAX || value < (int64_t)PELOTON_INT32_MIN) {
          throwCastSQLValueOutOfRangeException<int64_t>(value, VALUE_TYPE_BIGINT, VALUE_TYPE_INTEGER);
        }
        return static_cast<int32_t>(value);
      }
      default:
        throwCastSQLException(type, VALUE_TYPE_INTEGER);
        return 0; // NOT REACHED
    }
  }

  double castAsDoubleAndGetValue() const {
    assert(isNull() == false);

    const ValueType type = getValueType();

    switch (type) {
      case VALUE_TYPE_NULL:
        return DOUBLE_MIN;
      case VALUE_TYPE_TINYINT:
        return static_cast<double>(getTinyInt());
      case VALUE_TYPE_SMALLINT:
        return static_cast<double>(getSmallInt());
      case VALUE_TYPE_INTEGER:
        return static_cast<double>(getInteger());
      case VALUE_TYPE_ADDRESS:
        return static_cast<double>(getBigInt());
      case VALUE_TYPE_BIGINT:
        return static_cast<double>(getBigInt());
      case VALUE_TYPE_TIMESTAMP:
        return static_cast<double>(getTimestamp());
      case VALUE_TYPE_DOUBLE:
        return getDouble();
      case VALUE_TYPE_DECIMAL:
      {
        TTInt scaledValue = getDecimal();
        // we only deal with the decimal number within int64_t range here
        int64_t whole = narrowDecimalToBigInt(scaledValue);
        int64_t fractional = getFractionalPart(scaledValue);
        double retval;
        retval = static_cast<double>(whole) +
            (static_cast<double>(fractional)/static_cast<double>(kMaxScaleFactor));
        return retval;
      }
      case VALUE_TYPE_VARCHAR:
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_DOUBLE);
        return 0; // NOT REACHED
    }
  }

  TTInt castAsDecimalAndGetValue() const {
    assert(isNull() == false);

    const ValueType type = getValueType();

    switch (type) {
      case VALUE_TYPE_TINYINT:
      case VALUE_TYPE_SMALLINT:
      case VALUE_TYPE_INTEGER:
      case VALUE_TYPE_BIGINT:
      case VALUE_TYPE_TIMESTAMP: {
        int64_t value = castAsBigIntAndGetValue();
        TTInt retval(value);
        retval *= kMaxScaleFactor;
        return retval;
      }
      case VALUE_TYPE_DECIMAL:
        return getDecimal();
      case VALUE_TYPE_DOUBLE: {
        int64_t intValue = castAsBigIntAndGetValue();
        TTInt retval(intValue);
        retval *= kMaxScaleFactor;

        double value = getDouble();
        value -= static_cast<double>(intValue); // isolate decimal part
        value *= static_cast<double>(kMaxScaleFactor); // scale up to integer.
        TTInt fracval((int64_t)value);
        retval += fracval;
        return retval;
      }
      case VALUE_TYPE_VARCHAR:
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_DECIMAL);
        return 0; // NOT REACHED
    }
  }

  /**
   * This funciton does not check NULL value.
   */
  double getNumberFromString() const
  {
    assert(isNull() == false);

    const int32_t strLength = getObjectLength_withoutNull();
    // Guarantee termination at end of object -- or strtod might not stop there.
    char safeBuffer[strLength+1];
    memcpy(safeBuffer, getObjectValue_withoutNull(), strLength);
    safeBuffer[strLength] = '\0';
    char * bufferEnd = safeBuffer;
    double result = strtod(safeBuffer, &bufferEnd);
    // Needs to have consumed SOMETHING.
    if (bufferEnd > safeBuffer) {
      // Unconsumed trailing chars are OK if they are whitespace.
      while (bufferEnd < safeBuffer+strLength && isspace(*bufferEnd)) {
        ++bufferEnd;
      }
      if (bufferEnd == safeBuffer+strLength) {
        return result;
      }
    }

    std::ostringstream oss;
    oss << "Could not convert to number: '" << safeBuffer << "' contains invalid character value.";
    throw Exception(oss.str());
  }

  Value castAsBigInt() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_BIGINT);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getBigInt() = static_cast<int64_t>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        retval.getBigInt() = static_cast<int64_t>(getSmallInt()); break;
      case VALUE_TYPE_INTEGER:
        retval.getBigInt() = static_cast<int64_t>(getInteger()); break;
      case VALUE_TYPE_ADDRESS:
        retval.getBigInt() = getBigInt(); break;
      case VALUE_TYPE_BIGINT:
        return *this;
      case VALUE_TYPE_TIMESTAMP:
        retval.getBigInt() = getTimestamp(); break;
      case VALUE_TYPE_DOUBLE:
        if (getDouble() > (double)INT64_MAX || getDouble() < (double)PELOTON_INT64_MIN) {
          throwCastSQLValueOutOfRangeException<double>(getDouble(), VALUE_TYPE_DOUBLE, VALUE_TYPE_BIGINT);
        }
        retval.getBigInt() = static_cast<int64_t>(getDouble()); break;
      case VALUE_TYPE_DECIMAL: {
        TTInt scaledValue = getDecimal();
        retval.getBigInt() = narrowDecimalToBigInt(scaledValue); break;
      }
      case VALUE_TYPE_VARCHAR:
        retval.getBigInt() = static_cast<int64_t>(getNumberFromString()); break;
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_BIGINT);
    }
    return retval;
  }

  Value castAsTimestamp() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_TIMESTAMP);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getTimestamp() = static_cast<int64_t>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        retval.getTimestamp() = static_cast<int64_t>(getSmallInt()); break;
      case VALUE_TYPE_INTEGER:
        retval.getTimestamp() = static_cast<int64_t>(getInteger()); break;
      case VALUE_TYPE_BIGINT:
        retval.getTimestamp() = getBigInt(); break;
      case VALUE_TYPE_TIMESTAMP:
        retval.getTimestamp() = getTimestamp(); break;
      case VALUE_TYPE_DOUBLE:
        // TODO: Consider just eliminating this switch case to throw a cast exception,
        // or explicitly throwing some other exception here.
        // Direct cast of double to timestamp (implemented via intermediate cast to integer, here)
        // is not a SQL standard requirement, may not even make it past the planner's type-checks,
        // or may just be too far a stretch.
        // OR it might be a convenience for some obscure system-generated edge case?

        if (getDouble() > (double)INT64_MAX || getDouble() < (double)PELOTON_INT64_MIN) {
          throwCastSQLValueOutOfRangeException<double>(getDouble(), VALUE_TYPE_DOUBLE, VALUE_TYPE_BIGINT);
        }
        retval.getTimestamp() = static_cast<int64_t>(getDouble()); break;
      case VALUE_TYPE_DECIMAL: {
        // TODO: Consider just eliminating this switch case to throw a cast exception,
        // or explicitly throwing some other exception here.
        // Direct cast of decimal to timestamp (implemented via intermediate cast to integer, here)
        // is not a SQL standard requirement, may not even make it past the planner's type-checks,
        // or may just be too far a stretch.
        // OR it might be a convenience for some obscure system-generated edge case?

        TTInt scaledValue = getDecimal();
        retval.getTimestamp() = narrowDecimalToBigInt(scaledValue); break;
      }
      case VALUE_TYPE_VARCHAR: {
        const int32_t length = getObjectLength_withoutNull();
        const char* bytes = reinterpret_cast<const char*>(getObjectValue_withoutNull());
        const std::string value(bytes, length);
        retval.getTimestamp() = parseTimestampString(value);
        break;
      }
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_TIMESTAMP);
    }
    return retval;
  }

  template <typename T>
  void narrowToInteger(const T value, ValueType sourceType)
  {
    if (value > (T)INT32_MAX || value < (T)PELOTON_INT32_MIN) {
      throwCastSQLValueOutOfRangeException(value, sourceType, VALUE_TYPE_INTEGER);
    }
    getInteger() = static_cast<int32_t>(value);
  }

  Value castAsInteger() const {
    Value retval(VALUE_TYPE_INTEGER);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getInteger() = static_cast<int32_t>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        retval.getInteger() = static_cast<int32_t>(getSmallInt()); break;
      case VALUE_TYPE_INTEGER:
        return *this;
      case VALUE_TYPE_BIGINT:
        retval.narrowToInteger(getBigInt(), type); break;
      case VALUE_TYPE_TIMESTAMP:
        retval.narrowToInteger(getTimestamp(), type); break;
      case VALUE_TYPE_DOUBLE:
        retval.narrowToInteger(getDouble(), type); break;
      case VALUE_TYPE_DECIMAL: {
        TTInt scaledValue = getDecimal();
        // get the whole part of the decimal
        int64_t whole = narrowDecimalToBigInt(scaledValue);
        // try to convert the whole part, which is a int64_t
        retval.narrowToInteger(whole, VALUE_TYPE_BIGINT); break;
      }
      case VALUE_TYPE_VARCHAR:
        retval.narrowToInteger(getNumberFromString(), type); break;
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_INTEGER);
    }
    return retval;
  }

  template <typename T>
  void narrowToSmallInt(const T value, ValueType sourceType)
  {
    if (value > (T)INT16_MAX || value < (T)PELOTON_INT16_MIN) {
      throwCastSQLValueOutOfRangeException(value, sourceType, VALUE_TYPE_SMALLINT);
    }
    getSmallInt() = static_cast<int16_t>(value);
  }

  Value castAsSmallInt() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_SMALLINT);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getSmallInt() = static_cast<int16_t>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        retval.getSmallInt() = getSmallInt(); break;
      case VALUE_TYPE_INTEGER:
        retval.narrowToSmallInt(getInteger(), type); break;
      case VALUE_TYPE_BIGINT:
        retval.narrowToSmallInt(getBigInt(), type); break;
      case VALUE_TYPE_TIMESTAMP:
        retval.narrowToSmallInt(getTimestamp(), type); break;
      case VALUE_TYPE_DOUBLE:
        retval.narrowToSmallInt(getDouble(), type); break;
      case VALUE_TYPE_DECIMAL: {
        TTInt scaledValue = getDecimal();
        int64_t whole = narrowDecimalToBigInt(scaledValue);
        retval.narrowToSmallInt(whole, VALUE_TYPE_BIGINT); break;
      }
      case VALUE_TYPE_VARCHAR:
        retval.narrowToSmallInt(getNumberFromString(), type); break;
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_SMALLINT);
    }
    return retval;
  }

  template <typename T>
  void narrowToTinyInt(const T value, ValueType sourceType)
  {
    if (value > (T)INT8_MAX || value < (T)PELOTON_INT8_MIN) {
      throwCastSQLValueOutOfRangeException(value, sourceType, VALUE_TYPE_TINYINT);
    }
    getTinyInt() = static_cast<int8_t>(value);
  }

  Value castAsTinyInt() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_TINYINT);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getTinyInt() = getTinyInt(); break;
      case VALUE_TYPE_SMALLINT:
        retval.narrowToTinyInt(getSmallInt(), type); break;
      case VALUE_TYPE_INTEGER:
        retval.narrowToTinyInt(getInteger(), type); break;
      case VALUE_TYPE_BIGINT:
        retval.narrowToTinyInt(getBigInt(), type); break;
      case VALUE_TYPE_TIMESTAMP:
        retval.narrowToTinyInt(getTimestamp(), type); break;
      case VALUE_TYPE_DOUBLE:
        retval.narrowToTinyInt(getDouble(), type); break;
      case VALUE_TYPE_DECIMAL: {
        TTInt scaledValue = getDecimal();
        int64_t whole = narrowDecimalToBigInt(scaledValue);
        retval.narrowToTinyInt(whole, type); break;
      }
      case VALUE_TYPE_VARCHAR:
        retval.narrowToTinyInt(getNumberFromString(), type); break;
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_TINYINT);
    }
    return retval;
  }

  Value castAsDouble() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_DOUBLE);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        retval.getDouble() = static_cast<double>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        retval.getDouble() = static_cast<double>(getSmallInt()); break;
      case VALUE_TYPE_INTEGER:
        retval.getDouble() = static_cast<double>(getInteger()); break;
      case VALUE_TYPE_BIGINT:
        retval.getDouble() = static_cast<double>(getBigInt()); break;
      case VALUE_TYPE_TIMESTAMP:
        retval.getDouble() = static_cast<double>(getTimestamp()); break;
      case VALUE_TYPE_DOUBLE:
        retval.getDouble() = getDouble(); break;
      case VALUE_TYPE_DECIMAL:
        retval.getDouble() = castAsDoubleAndGetValue(); break;
      case VALUE_TYPE_VARCHAR:
        retval.getDouble() = getNumberFromString(); break;
      case VALUE_TYPE_VARBINARY:
      default:
        throwCastSQLException(type, VALUE_TYPE_DOUBLE);
    }
    return retval;
  }

  void streamTimestamp(std::stringstream& value) const;

  Value castAsString() const {
    assert(isNull() == false);

    std::stringstream value;
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_TINYINT:
        // This cast keeps the tiny int from being confused for a char.
        value << static_cast<int>(getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        value << getSmallInt(); break;
      case VALUE_TYPE_INTEGER:
        value << getInteger(); break;
      case VALUE_TYPE_BIGINT:
        value << getBigInt(); break;
        //case VALUE_TYPE_TIMESTAMP:
        //TODO: The SQL standard wants an actual date literal rather than a numeric value, here. See ENG-4284.
        //value << static_cast<double>(getTimestamp()); break;
      case VALUE_TYPE_DOUBLE:
        // Use the specific standard SQL formatting for float values,
        // which the C/C++ format options don't quite support.
        streamSQLFloatFormat(value, getDouble());
        break;
      case VALUE_TYPE_DECIMAL:
        value << createStringFromDecimal(); break;
      case VALUE_TYPE_VARCHAR:
      case VALUE_TYPE_VARBINARY: {
        // note: we allow binary conversion to strings to support
        // byte[] as string parameters...
        // In the future, it would be nice to check this is a decent string here...
        Value retval(VALUE_TYPE_VARCHAR);
        memcpy(retval.m_data, m_data, sizeof(m_data));
        return retval;
      }
      case VALUE_TYPE_TIMESTAMP: {
        streamTimestamp(value);
        break;
      }
      default:
        throwCastSQLException(type, VALUE_TYPE_VARCHAR);
    }
    return getTempStringValue(value.str().c_str(), value.str().length());
  }

  Value castAsBinary() const {
    assert(isNull() == false);

    Value retval(VALUE_TYPE_VARBINARY);
    const ValueType type = getValueType();
    switch (type) {
      case VALUE_TYPE_VARBINARY:
        memcpy(retval.m_data, m_data, sizeof(m_data));
        break;
      default:
        throwCastSQLException(type, VALUE_TYPE_VARBINARY);
    }
    return retval;
  }

  void createDecimalFromInt(int64_t rhsint)
  {
    TTInt scaled(rhsint);
    scaled *= kMaxScaleFactor;
    getDecimal() = scaled;
  }

  Value castAsDecimal() const {
    assert(isNull() == false);
    Value retval(VALUE_TYPE_DECIMAL);
    const ValueType type = getValueType();
    if (isNull()) {
      retval.setNull();
      return retval;
    }
    switch (type) {
      case VALUE_TYPE_TINYINT:
      case VALUE_TYPE_SMALLINT:
      case VALUE_TYPE_INTEGER:
      case VALUE_TYPE_BIGINT:
      {
        int64_t rhsint = castAsBigIntAndGetValue();
        retval.createDecimalFromInt(rhsint);
        break;
      }
      case VALUE_TYPE_DECIMAL:
        ::memcpy(retval.m_data, m_data, sizeof(TTInt));
        break;
      case VALUE_TYPE_DOUBLE:
      {
        const double& value = getDouble();
        if (value >= s_gtMaxDecimalAsDouble || value <= s_ltMinDecimalAsDouble) {
          char message[4096];
          snprintf(message, 4096, "Attempted to cast value %f causing overflow/underflow", value);
          throw Exception(message);
        }
        // Resort to string as the intermediary since even int64_t does not cover the full range.
        char decimalAsString[41]; // Large enough to account for digits, sign, decimal, and terminating null.
        snprintf(decimalAsString, sizeof(decimalAsString), "%.12f", value);
        // Shift the entire integer part 1 digit to the right, overwriting the decimal point.
        // This effectively creates a potentially very large integer value
        //  equal to the original double scaled up by 10^12.
        for (char* intDigit = strchr(decimalAsString, '.'); intDigit > decimalAsString; --intDigit) {
          *intDigit = *(intDigit-1);
        }
        TTInt result(decimalAsString+1);
        retval.getDecimal() = result;
        break;
      }
      case VALUE_TYPE_VARCHAR:
      {
        const int32_t length = getObjectLength_withoutNull();
        const char* bytes = reinterpret_cast<const char*>(getObjectValue_withoutNull());
        const std::string value(bytes, length);
        retval.createDecimalFromString(value);
        break;
      }
      default:
        throwCastSQLException(type, VALUE_TYPE_DECIMAL);
    }
    return retval;
  }

  /**
   * Copy the arbitrary size object that this value points to as an
   * inline object in the provided storage area
   */
  void inlineCopyObject(void *storage, int32_t maxLength, bool isInBytes) const {
    if (isNull()) {
      // Always reset all the bits regardless of the actual length of the value
      // 1 additional byte for the length prefix
      ::memset(storage, 0, maxLength + 1);

      /*
       * The 7th bit of the length preceding value
       * is used to indicate that the object is null.
       */
      *reinterpret_cast<char*>(storage) = OBJECT_NULL_BIT;
    }
    else {
      const int32_t objLength = getObjectLength_withoutNull();
      const char* ptr = reinterpret_cast<const char*>(getObjectValue_withoutNull());
      checkTooNarrowVarcharAndVarbinary(m_valueType, ptr, objLength, maxLength, isInBytes);

      // Always reset all the bits regardless of the actual length of the value
      // 1 additional byte for the length prefix
      ::memset(storage, 0, maxLength + 1);

      if (m_sourceInlined)
      {
        ::memcpy( storage, *reinterpret_cast<char *const *>(m_data), getObjectLengthLength() + objLength);
      }
      else
      {
        const Varlen* sref =
            *reinterpret_cast<Varlen* const*>(m_data);
        ::memcpy(storage, sref->Get(),
                 getObjectLengthLength() + objLength);
      }
    }

  }

  static inline bool validVarcharSize(const char *valueChars, const size_t length, const int32_t maxLength) {
    int32_t min_continuation_bytes = static_cast<int32_t>(length - maxLength);
    if (min_continuation_bytes <= 0) {
      return true;
    }
    size_t i = length;
    while (i--) {
      if ((valueChars[i] & 0xc0) == 0x80) {
        if (--min_continuation_bytes == 0) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * Assuming non-null Value, validate the size of the varchar or varbinary
   */
  static inline void checkTooNarrowVarcharAndVarbinary(ValueType type, const char* ptr,
                                                       int32_t objLength, int32_t maxLength, bool isInBytes) {
    if (maxLength == 0) {
      throw ObjectSizeException("Zero maxLength for object type" + ValueTypeToString(type));
    }

    if (type == VALUE_TYPE_VARBINARY) {
      if (objLength > maxLength) {
        char msg[1024];
        snprintf(msg, 1024,
                 "The size %d of the value exceeds the size of the VARBINARY(%d) column.",
                 objLength, maxLength);
        throw ObjectSizeException(msg);
      }
    } else if (type == VALUE_TYPE_VARCHAR) {
      if (isInBytes) {
        if (objLength > maxLength) {
          std::string inputValue;
          if (objLength > FULL_STRING_IN_MESSAGE_THRESHOLD) {
            inputValue = std::string(ptr, FULL_STRING_IN_MESSAGE_THRESHOLD) + std::string("...");
          } else {
            inputValue = std::string(ptr, objLength);
          }
          char msg[1024];
          snprintf(msg, 1024,
                   "The size %d of the value '%s' exceeds the size of the VARCHAR(%d BYTES) column.",
                   objLength, inputValue.c_str(), maxLength);
          throw ObjectSizeException(msg);
        }
      } else if (!validVarcharSize(ptr, objLength, maxLength)) {
        const int32_t charLength = getCharLength(ptr, objLength);
        char msg[1024];
        std::string inputValue;
        if (charLength > FULL_STRING_IN_MESSAGE_THRESHOLD) {
          const char * end = getIthCharPosition(ptr, objLength, FULL_STRING_IN_MESSAGE_THRESHOLD+1);
          int32_t numBytes = (int32_t)(end - ptr);
          inputValue = std::string(ptr, numBytes) + std::string("...");
        } else {
          inputValue = std::string(ptr, objLength);
        }
        snprintf(msg, 1024,
                 "The size %d of the value '%s' exceeds the size of the VARCHAR(%d) column.",
                 charLength, inputValue.c_str(), maxLength);

        throw ObjectSizeException(msg);
      }
    } else {
      throw IncompatibleTypeException(type, "Invalid object type for this function");
    }
  }

  template<typename T>
  int compareValue (const T lhsValue, const T rhsValue) const {
    if (lhsValue == rhsValue) {
      return VALUE_COMPARE_EQUAL;
    } else if (lhsValue > rhsValue) {
      return VALUE_COMPARE_GREATERTHAN;
    } else {
      return VALUE_COMPARE_LESSTHAN;
    }
  }

  int compareDoubleValue (const double lhsValue, const double rhsValue) const {
    // Treat NaN values as equals and also make them smaller than neagtive infinity.
    // This breaks IEEE754 for expressions slightly.
    if (std::isnan(lhsValue)) {
      return std::isnan(rhsValue) ? VALUE_COMPARE_EQUAL : VALUE_COMPARE_LESSTHAN;
    }
    else if (std::isnan(rhsValue)) {
      return VALUE_COMPARE_GREATERTHAN;
    }
    else if (lhsValue > rhsValue) {
      return VALUE_COMPARE_GREATERTHAN;
    }
    else if (lhsValue < rhsValue) {
      return VALUE_COMPARE_LESSTHAN;
    }
    else {
      return VALUE_COMPARE_EQUAL;
    }
  }

  int compareTinyInt (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_TINYINT);

    // get the right hand side as a bigint
    if (rhs.getValueType() == VALUE_TYPE_DOUBLE) {
      return compareDoubleValue(static_cast<double>(getTinyInt()), rhs.getDouble());
    } else if (rhs.getValueType() == VALUE_TYPE_DECIMAL) {
      const TTInt rhsValue = rhs.getDecimal();
      TTInt lhsValue(static_cast<int64_t>(getTinyInt()));
      lhsValue *= kMaxScaleFactor;
      return compareValue<TTInt>(lhsValue, rhsValue);
    } else {
      int64_t lhsValue, rhsValue;
      lhsValue = static_cast<int64_t>(getTinyInt());
      rhsValue = rhs.castAsBigIntAndGetValue();
      return compareValue<int64_t>(lhsValue, rhsValue);
    }
  }

  int compareSmallInt (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_SMALLINT);

    // get the right hand side as a bigint
    if (rhs.getValueType() == VALUE_TYPE_DOUBLE) {
      return compareDoubleValue(static_cast<double>(getSmallInt()), rhs.getDouble());
    } else if (rhs.getValueType() == VALUE_TYPE_DECIMAL) {
      const TTInt rhsValue = rhs.getDecimal();
      TTInt lhsValue(static_cast<int64_t>(getSmallInt()));
      lhsValue *= kMaxScaleFactor;
      return compareValue<TTInt>(lhsValue, rhsValue);
    } else {
      int64_t lhsValue, rhsValue;
      lhsValue = static_cast<int64_t>(getSmallInt());
      rhsValue = rhs.castAsBigIntAndGetValue();
      return compareValue<int64_t>(lhsValue, rhsValue);
    }
  }

  int compareInteger (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_INTEGER);

    // get the right hand side as a bigint
    if (rhs.getValueType() == VALUE_TYPE_DOUBLE) {
      return compareDoubleValue(static_cast<double>(getInteger()), rhs.getDouble());
    } else if (rhs.getValueType() == VALUE_TYPE_DECIMAL) {
      const TTInt rhsValue = rhs.getDecimal();
      TTInt lhsValue(static_cast<int64_t>(getInteger()));
      lhsValue *= kMaxScaleFactor;
      return compareValue<TTInt>(lhsValue, rhsValue);
    } else {
      int64_t lhsValue, rhsValue;
      lhsValue = static_cast<int64_t>(getInteger());
      rhsValue = rhs.castAsBigIntAndGetValue();
      return compareValue<int64_t>(lhsValue, rhsValue);
    }
  }

  int compareBigInt (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_BIGINT);

    // get the right hand side as a bigint
    if (rhs.getValueType() == VALUE_TYPE_DOUBLE) {
      return compareDoubleValue(static_cast<double>(getBigInt()), rhs.getDouble());
    } else if (rhs.getValueType() == VALUE_TYPE_DECIMAL) {
      const TTInt rhsValue = rhs.getDecimal();
      TTInt lhsValue(getBigInt());
      lhsValue *= kMaxScaleFactor;
      return compareValue<TTInt>(lhsValue, rhsValue);
    } else {
      int64_t lhsValue, rhsValue;
      lhsValue = getBigInt();
      rhsValue = rhs.castAsBigIntAndGetValue();
      return compareValue<int64_t>(lhsValue, rhsValue);
    }
  }


  int compareTimestamp (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_TIMESTAMP);

    // get the right hand side as a bigint
    if (rhs.getValueType() == VALUE_TYPE_DOUBLE) {
      return compareDoubleValue(static_cast<double>(getTimestamp()), rhs.getDouble());
    } else if (rhs.getValueType() == VALUE_TYPE_DECIMAL) {
      const TTInt rhsValue = rhs.getDecimal();
      TTInt lhsValue(getTimestamp());
      lhsValue *= kMaxScaleFactor;
      return compareValue<TTInt>(lhsValue, rhsValue);
    } else {
      int64_t lhsValue, rhsValue;
      lhsValue = getTimestamp();
      rhsValue = rhs.castAsBigIntAndGetValue();
      return compareValue<int64_t>(lhsValue, rhsValue);
    }
  }

  int compareDoubleValue (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_DOUBLE);

    const double lhsValue = getDouble();
    double rhsValue;

    switch (rhs.getValueType()) {
      case VALUE_TYPE_DOUBLE:
        rhsValue = rhs.getDouble(); break;
      case VALUE_TYPE_TINYINT:
        rhsValue = static_cast<double>(rhs.getTinyInt()); break;
      case VALUE_TYPE_SMALLINT:
        rhsValue = static_cast<double>(rhs.getSmallInt()); break;
      case VALUE_TYPE_INTEGER:
        rhsValue = static_cast<double>(rhs.getInteger()); break;
      case VALUE_TYPE_BIGINT:
        rhsValue = static_cast<double>(rhs.getBigInt()); break;
      case VALUE_TYPE_TIMESTAMP:
        rhsValue = static_cast<double>(rhs.getTimestamp()); break;
      case VALUE_TYPE_DECIMAL:
      {
        TTInt scaledValue = rhs.getDecimal();
        TTInt whole(scaledValue);
        TTInt fractional(scaledValue);
        whole /= kMaxScaleFactor;
        fractional %= kMaxScaleFactor;
        rhsValue = static_cast<double>(whole.ToInt()) +
            (static_cast<double>(fractional.ToInt())/static_cast<double>(kMaxScaleFactor));
        break;
      }
      default:
        char message[128];
        snprintf(message, 128,
                 "Type %s cannot be cast for comparison to type %s",
                 ValueTypeToString(rhs.getValueType()).c_str(),
                 ValueTypeToString(getValueType()).c_str());
        throw TypeMismatchException(message, rhs.getValueType(), getValueType());
        // Not reached
    }

    return compareDoubleValue(lhsValue, rhsValue);
  }

  int compareStringValue (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_VARCHAR);

    ValueType rhsType = rhs.getValueType();
    if ((rhsType != VALUE_TYPE_VARCHAR) && (rhsType != VALUE_TYPE_VARBINARY)) {
      char message[128];
      snprintf(message, 128,
               "Type %s cannot be cast for comparison to type %s",
               ValueTypeToString(rhsType).c_str(),
               ValueTypeToString(m_valueType).c_str());
      throw TypeMismatchException(message, rhs.getValueType(), getValueType());
    }

    assert(m_valueType == VALUE_TYPE_VARCHAR);

    const int32_t leftLength = getObjectLength_withoutNull();
    const int32_t rightLength = rhs.getObjectLength_withoutNull();
    const char* left = reinterpret_cast<const char*>(getObjectValue_withoutNull());
    const char* right = reinterpret_cast<const char*>(rhs.getObjectValue_withoutNull());

    const int result = ::strncmp(left, right, std::min(leftLength, rightLength));
    if (result == 0 && leftLength != rightLength) {
      if (leftLength > rightLength) {
        return  VALUE_COMPARE_GREATERTHAN;
      } else {
        return VALUE_COMPARE_LESSTHAN;
      }
    }
    else if (result > 0) {
      return VALUE_COMPARE_GREATERTHAN;
    }
    else if (result < 0) {
      return VALUE_COMPARE_LESSTHAN;
    }

    return VALUE_COMPARE_EQUAL;
  }

  int compareBinaryValue (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_VARBINARY);

    if (rhs.getValueType() != VALUE_TYPE_VARBINARY) {
      char message[128];
      snprintf(message, 128,
               "Type %s cannot be cast for comparison to type %s",
               ValueTypeToString(rhs.getValueType()).c_str(),
               ValueTypeToString(m_valueType).c_str());
      throw TypeMismatchException(message, rhs.getValueType(), m_valueType);
    }
    const int32_t leftLength = getObjectLength_withoutNull();
    const int32_t rightLength = rhs.getObjectLength_withoutNull();

    const char* left = reinterpret_cast<const char*>(getObjectValue_withoutNull());
    const char* right = reinterpret_cast<const char*>(rhs.getObjectValue_withoutNull());

    const int result = ::memcmp(left, right, std::min(leftLength, rightLength));
    if (result == 0 && leftLength != rightLength) {
      if (leftLength > rightLength) {
        return  VALUE_COMPARE_GREATERTHAN;
      } else {
        return VALUE_COMPARE_LESSTHAN;
      }
    }
    else if (result > 0) {
      return VALUE_COMPARE_GREATERTHAN;
    }
    else if (result < 0) {
      return VALUE_COMPARE_LESSTHAN;
    }

    return VALUE_COMPARE_EQUAL;
  }

  int compareDecimalValue (const Value rhs) const {
    assert(m_valueType == VALUE_TYPE_DECIMAL);
    switch (rhs.getValueType()) {
      case VALUE_TYPE_DECIMAL:
      {
        return compareValue<TTInt>(getDecimal(), rhs.getDecimal());
      }
      case VALUE_TYPE_DOUBLE:
      {
        const double rhsValue = rhs.getDouble();
        TTInt scaledValue = getDecimal();
        TTInt whole(scaledValue);
        TTInt fractional(scaledValue);
        whole /= kMaxScaleFactor;
        fractional %= kMaxScaleFactor;
        const double lhsValue = static_cast<double>(whole.ToInt()) +
            (static_cast<double>(fractional.ToInt())/static_cast<double>(kMaxScaleFactor));

        return compareValue<double>(lhsValue, rhsValue);
      }
      // create the equivalent decimal value
      case VALUE_TYPE_TINYINT:
      {
        TTInt rhsValue(static_cast<int64_t>(rhs.getTinyInt()));
        rhsValue *= kMaxScaleFactor;
        return compareValue<TTInt>(getDecimal(), rhsValue);
      }
      case VALUE_TYPE_SMALLINT:
      {
        TTInt rhsValue(static_cast<int64_t>(rhs.getSmallInt()));
        rhsValue *= kMaxScaleFactor;
        return compareValue<TTInt>(getDecimal(), rhsValue);
      }
      case VALUE_TYPE_INTEGER:
      {
        TTInt rhsValue(static_cast<int64_t>(rhs.getInteger()));
        rhsValue *= kMaxScaleFactor;
        return compareValue<TTInt>(getDecimal(), rhsValue);
      }
      case VALUE_TYPE_BIGINT:
      {
        TTInt rhsValue(rhs.getBigInt());
        rhsValue *= kMaxScaleFactor;
        return compareValue<TTInt>(getDecimal(), rhsValue);
      }
      case VALUE_TYPE_TIMESTAMP:
      {
        TTInt rhsValue(rhs.getTimestamp());
        rhsValue *= kMaxScaleFactor;
        return compareValue<TTInt>(getDecimal(), rhsValue);
      }
      default:
      {
        char message[128];
        snprintf(message, 128,
                 "Type %s cannot be cast for comparison to type %s",
                 ValueTypeToString(rhs.getValueType()).c_str(),
                 ValueTypeToString(getValueType()).c_str());
        throw TypeMismatchException(message, rhs.getValueType(), getValueType());
        // Not reached
        return 0;
      }
    }
  }

  Value opAddBigInts(const int64_t lhs, const int64_t rhs) const {
    //Scary overflow check from https://www.securecoding.cert.org/confluence/display/cplusplus/INT32-CPP.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow
    if ( ((lhs^rhs)
        | (((lhs^(~(lhs^rhs)
            & (1L << (sizeof(int64_t)*CHAR_BIT-1))))+rhs)^rhs)) >= 0) {
      char message[4096];
      snprintf(message, 4096, "Adding %jd and %jd will overflow BigInt storage", (intmax_t)lhs, (intmax_t)rhs);
      throw Exception(message);
    }
    return getBigIntValue(lhs + rhs);
  }

  Value opSubtractBigInts(const int64_t lhs, const int64_t rhs) const {
    //Scary overflow check from https://www.securecoding.cert.org/confluence/display/cplusplus/INT32-CPP.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow
    if ( ((lhs^rhs)
        & (((lhs ^ ((lhs^rhs)
            & (1L << (sizeof(int64_t)*CHAR_BIT-1))))-rhs)^rhs)) < 0) {
      char message[4096];
      snprintf(message, 4096, "Subtracting %jd from %jd will overflow BigInt storage", (intmax_t)lhs, (intmax_t)rhs);
      throw Exception(message);
    }
    return getBigIntValue(lhs - rhs);
  }

  Value opMultiplyBigInts(const int64_t lhs, const int64_t rhs) const {
    bool overflow = false;
    //Scary overflow check from https://www.securecoding.cert.org/confluence/display/cplusplus/INT32-CPP.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow
    if (lhs > 0){  /* lhs is positive */
      if (rhs > 0) {  /* lhs and rhs are positive */
        if (lhs > (INT64_MAX / rhs)) {
          overflow= true;
        }
      } /* end if lhs and rhs are positive */
      else { /* lhs positive, rhs non-positive */
        if (rhs < (INT64_MIN / lhs)) {
          overflow = true;
        }
      } /* lhs positive, rhs non-positive */
    } /* end if lhs is positive */
    else { /* lhs is non-positive */
      if (rhs > 0) { /* lhs is non-positive, rhs is positive */
        if (lhs < (INT64_MIN / rhs)) {
          overflow = true;
        }
      } /* end if lhs is non-positive, rhs is positive */
      else { /* lhs and rhs are non-positive */
        if ( (lhs != 0) && (rhs < (INT64_MAX / lhs))) {
          overflow = true;
        }
      } /* end if lhs and rhs non-positive */
    } /* end if lhs is non-positive */

    const int64_t result = lhs * rhs;

    if (result == INT64_NULL) {
      overflow = true;
    }

    if (overflow) {
      char message[4096];
      snprintf(message, 4096, "Multiplying %jd with %jd will overflow BigInt storage", (intmax_t)lhs, (intmax_t)rhs);
      throw Exception(message);
    }

    return getBigIntValue(result);
  }

  Value opDivideBigInts(const int64_t lhs, const int64_t rhs) const {
    if (rhs == 0) {
      char message[4096];
      snprintf(message, 4096, "Attempted to divide %jd by 0", (intmax_t)lhs);
      throw Exception(message);
    }

    /**
     * Because the smallest int64 value is used to represent null (and this is checked for an handled above)
     * it isn't necessary to check for any kind of overflow since none is possible.
     */
    return getBigIntValue(int64_t(lhs / rhs));
  }

  Value opAddDoubles(const double lhs, const double rhs) const {
    const double result = lhs + rhs;
    throwDataExceptionIfInfiniteOrNaN(result, "'+' operator");
    return getDoubleValue(result);
  }

  Value opSubtractDoubles(const double lhs, const double rhs) const {
    const double result = lhs - rhs;
    throwDataExceptionIfInfiniteOrNaN(result, "'-' operator");
    return getDoubleValue(result);
  }

  Value opMultiplyDoubles(const double lhs, const double rhs) const {
    const double result = lhs * rhs;
    throwDataExceptionIfInfiniteOrNaN(result, "'*' operator");
    return getDoubleValue(result);
  }

  Value opDivideDoubles(const double lhs, const double rhs) const {
    const double result = lhs / rhs;
    throwDataExceptionIfInfiniteOrNaN(result, "'/' operator");
    return getDoubleValue(result);
  }

  Value opAddDecimals(const Value &lhs, const Value &rhs) const {
    assert(lhs.isNull() == false);
    assert(rhs.isNull() == false);
    assert(lhs.getValueType() == VALUE_TYPE_DECIMAL);
    assert(rhs.getValueType() == VALUE_TYPE_DECIMAL);

    TTInt retval(lhs.getDecimal());
    if (retval.Add(rhs.getDecimal()) || retval > s_maxDecimalValue || retval < s_minDecimalValue) {
      char message[4096];
      snprintf(message, 4096, "Attempted to add %s with %s causing overflow/underflow",
               lhs.createStringFromDecimal().c_str(), rhs.createStringFromDecimal().c_str());
      throw Exception(message);
    }

    return getDecimalValue(retval);
  }

  Value opSubtractDecimals(const Value &lhs, const Value &rhs) const {
    assert(lhs.isNull() == false);
    assert(rhs.isNull() == false);
    assert(lhs.getValueType() == VALUE_TYPE_DECIMAL);
    assert(rhs.getValueType() == VALUE_TYPE_DECIMAL);

    TTInt retval(lhs.getDecimal());
    if (retval.Sub(rhs.getDecimal()) || retval > s_maxDecimalValue || retval < s_minDecimalValue) {
      char message[4096];
      snprintf(message, 4096, "Attempted to subtract %s from %s causing overflow/underflow",
               rhs.createStringFromDecimal().c_str(), lhs.createStringFromDecimal().c_str());
      throw Exception(message);
    }

    return getDecimalValue(retval);
  }

  /*
   * Avoid scaling both sides if possible. E.g, don't turn dec * 2 into
   * (dec * 2*kMaxScale*E-12). Then the result of simple multiplication
   * is a*b*E-24 and have to further multiply to get back to the assumed
   * E-12, which can overflow unnecessarily at the middle step.
   */
  Value opMultiplyDecimals(const Value &lhs, const Value &rhs) const {
    assert(lhs.isNull() == false);
    assert(rhs.isNull() == false);
    assert(lhs.getValueType() == VALUE_TYPE_DECIMAL);
    assert(rhs.getValueType() == VALUE_TYPE_DECIMAL);

    TTLInt calc;
    calc.FromInt(lhs.getDecimal());
    calc *= rhs.getDecimal();
    calc /= kMaxScaleFactor;
    TTInt retval;
    if (retval.FromInt(calc)  || retval > s_maxDecimalValue || retval < s_minDecimalValue) {
      char message[4096];
      snprintf(message, 4096, "Attempted to multiply %s by %s causing overflow/underflow. Unscaled result was %s",
               lhs.createStringFromDecimal().c_str(), rhs.createStringFromDecimal().c_str(),
               calc.ToString(10).c_str());
      throw Exception(message);
    }
    return getDecimalValue(retval);
  }


  /*
   * Divide two decimals and return a correctly scaled decimal.
   * A little cumbersome. Better algorithms welcome.
   *   (1) calculate the quotient and the remainder.
   *   (2) temporarily scale the remainder to 19 digits
   *   (3) divide out remainder to calculate digits after the radix point.
   *   (4) scale remainder to 12 digits (that's the default scale)
   *   (5) scale the quotient back to 19,12.
   *   (6) sum the scaled quotient and remainder.
   *   (7) construct the final decimal.
   */

  Value opDivideDecimals(const Value &lhs, const Value &rhs) const {
    assert(lhs.isNull() == false);
    assert(rhs.isNull() == false);
    assert(lhs.getValueType() == VALUE_TYPE_DECIMAL);
    assert(rhs.getValueType() == VALUE_TYPE_DECIMAL);

    TTLInt calc;
    calc.FromInt(lhs.getDecimal());
    calc *= kMaxScaleFactor;
    if (calc.Div(rhs.getDecimal())) {
      char message[4096];
      snprintf( message, 4096, "Attempted to divide %s by %s causing overflow/underflow (or divide by zero)",
                lhs.createStringFromDecimal().c_str(), rhs.createStringFromDecimal().c_str());
      throw Exception(message);
    }
    TTInt retval;
    if (retval.FromInt(calc)  || retval > s_maxDecimalValue || retval < s_minDecimalValue) {
      char message[4096];
      snprintf( message, 4096, "Attempted to divide %s by %s causing overflow. Unscaled result was %s",
                lhs.createStringFromDecimal().c_str(), rhs.createStringFromDecimal().c_str(),
                calc.ToString(10).c_str());
      throw Exception(message);
    }
    return getDecimalValue(retval);
  }

  static Value getTinyIntValue(int8_t value) {
    Value retval(VALUE_TYPE_TINYINT);
    retval.getTinyInt() = value;
    if (value == INT8_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getSmallIntValue(int16_t value) {
    Value retval(VALUE_TYPE_SMALLINT);
    retval.getSmallInt() = value;
    if (value == INT16_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getIntegerValue(int32_t value) {
    Value retval(VALUE_TYPE_INTEGER);
    retval.getInteger() = value;
    if (value == INT32_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getBigIntValue(int64_t value) {
    Value retval(VALUE_TYPE_BIGINT);
    retval.getBigInt() = value;
    if (value == INT64_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getTimestampValue(int64_t value) {
    Value retval(VALUE_TYPE_TIMESTAMP);
    retval.getTimestamp() = value;
    if (value == INT64_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getDoubleValue(double value) {
    Value retval(VALUE_TYPE_DOUBLE);
    retval.getDouble() = value;
    if (value <= DOUBLE_NULL) {
      retval.tagAsNull();
    }
    return retval;
  }

  static Value getBooleanValue(bool value) {
    Value retval(VALUE_TYPE_BOOLEAN);
    retval.getBoolean() = value;
    return retval;
  }

  static Value getDecimalValueFromString(const std::string &value) {
    Value retval(VALUE_TYPE_DECIMAL);
    retval.createDecimalFromString(value);
    return retval;
  }

  static Value getAllocatedArrayValueFromSizeAndType(size_t elementCount, ValueType elementType)
  {
    Value retval(VALUE_TYPE_ARRAY);
    retval.allocateANewValueList(elementCount, elementType);
    return retval;
  }

  static VarlenPool* getTempStringPool();

  static Value getTempStringValue(const char* value, size_t size) {
    return getAllocatedValue(VALUE_TYPE_VARCHAR, value, size, getTempStringPool());
  }

  static Value getTempBinaryValue(const unsigned char* value, size_t size) {
    return getAllocatedValue(VALUE_TYPE_VARBINARY, reinterpret_cast<const char*>(value), size, getTempStringPool());
  }

  /// Assumes hex-encoded input
  static inline Value getTempBinaryValueFromHex(const std::string& value) {
    size_t rawLength = value.length() / 2;
    unsigned char rawBuf[rawLength];
    HexDecodeToBinary(rawBuf, value.c_str());
    return getTempBinaryValue(rawBuf, rawLength);
  }

  static Value getAllocatedValue(ValueType type, const char* value, size_t size, VarlenPool* stringPool) {
    Value retval(type);
    char* storage = retval.allocateValueStorage((int32_t)size, stringPool);
    ::memcpy(storage, value, (int32_t)size);
    return retval;
  }

  char* allocateValueStorage(int32_t length, VarlenPool* stringPool)
  {
    // This unsets the Value's null tag and returns the length of the length.
    const int8_t lengthLength = setObjectLength(length);
    const int32_t minLength = length + lengthLength;
    Varlen* sref = Varlen::Create(minLength, stringPool);
    char* storage = sref->Get();
    setObjectLengthToLocation(length, storage);
    storage += lengthLength;
    setObjectValue(sref);
    return storage;
  }

  static Value getNullStringValue() {
    Value retval(VALUE_TYPE_VARCHAR);
    retval.tagAsNull();
    *reinterpret_cast<char**>(retval.m_data) = NULL;
    return retval;
  }

  static Value getNullBinaryValue() {
    Value retval(VALUE_TYPE_VARBINARY);
    retval.tagAsNull();
    *reinterpret_cast<char**>(retval.m_data) = NULL;
    return retval;
  }

  static Value getNullValue() {
    Value retval(VALUE_TYPE_NULL);
    retval.tagAsNull();
    return retval;
  }

  static Value getDecimalValue(TTInt value) {
    Value retval(VALUE_TYPE_DECIMAL);
    retval.getDecimal() = value;
    return retval;
  }

  static Value getAddressValue(void *address) {
    Value retval(VALUE_TYPE_ADDRESS);
    *reinterpret_cast<void**>(retval.m_data) = address;
    return retval;
  }

  /// Common code to implement variants of the TRIM SQL function: LEADING, TRAILING, or BOTH
  static Value trimWithOptions(const std::vector<Value>& arguments, bool leading, bool trailing);

};

/**
 * Public constructor that initializes to an Value that is unusable
 * with other Values.  Useful for declaring storage for an Value.
 */
inline Value::Value() {
  ::memset( m_data, 0, 16);
  setValueType(VALUE_TYPE_INVALID);
  m_sourceInlined = false;
}

/**
 * Retrieve a boolean Value that is true
 */
inline Value Value::getTrue() {
  Value retval(VALUE_TYPE_BOOLEAN);
  retval.getBoolean() = true;
  return retval;
}

/**
 * Retrieve a boolean Value that is false
 */
inline Value Value::getFalse() {
  Value retval(VALUE_TYPE_BOOLEAN);
  retval.getBoolean() = false;
  return retval;
}

/**
 * Returns C++ true if this Value is a boolean and is true
 * If it is NULL, return false.
 */
inline bool Value::isTrue() const {
  if (isBooleanNULL()) {
    return false;
  }
  return getBoolean();
}

/**
 * Returns C++ false if this Value is a boolean and is true
 * If it is NULL, return false.
 */
inline bool Value::isFalse() const {
  if (isBooleanNULL()) {
    return false;
  }
  return !getBoolean();
}

inline bool Value::isBooleanNULL() const {
  assert(getValueType() == VALUE_TYPE_BOOLEAN);
  return *reinterpret_cast<const int8_t*>(m_data) == INT8_NULL;
}

inline bool Value::getSourceInlined() const {
  return m_sourceInlined;
}

/**
 * Objects may have storage allocated for them. Calling free causes the Value to return the storage allocated for
 * the object to the heap
 */
inline void Value::free() const {
  switch (getValueType())
  {
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    case VALUE_TYPE_ARRAY:
    {
      assert(!m_sourceInlined);
      Varlen* sref = *reinterpret_cast<Varlen* const*>(m_data);
      if (sref != NULL)
      {
        Varlen::Destroy(sref);
      }
    }
    break;
    default:
      return;
  }
}

inline void Value::freeObjectsFromTupleStorage(std::vector<char*> const &oldObjects)
{

  for (std::vector<char*>::const_iterator it = oldObjects.begin(); it != oldObjects.end(); ++it) {
    Varlen* sref = reinterpret_cast<Varlen*>(*it);
    if (sref != NULL) {
      Varlen::Destroy(sref);
    }
  }
}

/**
 * Get the amount of storage necessary to store a value of the specified type
 * in a tuple
 */
inline uint16_t Value::getTupleStorageSize(const ValueType type) {
  switch (type) {
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return sizeof(int64_t);
    case VALUE_TYPE_TINYINT:
      return sizeof(int8_t);
    case VALUE_TYPE_SMALLINT:
      return sizeof(int16_t);
    case VALUE_TYPE_INTEGER:
      return sizeof(int32_t);
    case VALUE_TYPE_DOUBLE:
      return sizeof(double);
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
      return sizeof(char*);
    case VALUE_TYPE_DECIMAL:
      return sizeof(TTInt);
    case VALUE_TYPE_BOOLEAN:
      return sizeof(bool);
    default:
      char message[128];
      snprintf(message, 128, "Value::getTupleStorageSize() unsupported type '%s'",
               ValueTypeToString(type).c_str());
      throw Exception(
          message);
  }
}

/**
 * This null compare function works for GROUP BY, ORDER BY, INDEX KEY, etc,
 * except for comparison expression.
 * comparison expression has different logic for null.
 */
inline int Value::compareNull(const Value rhs) const {
  bool lnull = isNull();
  bool rnull = rhs.isNull();

  if (lnull) {
    if (rnull) {
      return VALUE_COMPARE_EQUAL;
    } else {
      return VALUE_COMPARE_LESSTHAN;
    }
  } else if (rnull) {
    return VALUE_COMPARE_GREATERTHAN;
  }
  return VALUE_COMPARE_INVALID;
}

/**
 * Assuming no nulls are in comparison.
 * Compare any two Values. Comparison is not guaranteed to
 * succeed if the values are incompatible.  Avoid use of
 * comparison in favor of op_*.
 */
inline int Value::compare_withoutNull(const Value rhs) const {
  assert(isNull() == false && rhs.isNull() == false);

  switch (m_valueType) {
    case VALUE_TYPE_VARCHAR:
      return compareStringValue(rhs);
    case VALUE_TYPE_BIGINT:
      return compareBigInt(rhs);
    case VALUE_TYPE_INTEGER:
      return compareInteger(rhs);
    case VALUE_TYPE_SMALLINT:
      return compareSmallInt(rhs);
    case VALUE_TYPE_TINYINT:
      return compareTinyInt(rhs);
    case VALUE_TYPE_TIMESTAMP:
      return compareTimestamp(rhs);
    case VALUE_TYPE_DOUBLE:
      return compareDoubleValue(rhs);
    case VALUE_TYPE_VARBINARY:
      return compareBinaryValue(rhs);
    case VALUE_TYPE_DECIMAL:
      return compareDecimalValue(rhs);
    default: {
      throw Exception("non comparable types :: " +  getValueTypeString() + rhs.getValueTypeString());
    }
    /* no break */
  }
}

/**
 * Compare any two Values. Comparison is not guaranteed to
 * succeed if the values are incompatible.  Avoid use of
 * comparison in favor of op_*.
 */
inline int Value::compare(const Value rhs) const {
  int hasNullCompare = compareNull(rhs);
  if (hasNullCompare != VALUE_COMPARE_INVALID) {
    return hasNullCompare;
  }

  return compare_withoutNull(rhs);
}

/**
 * Set this Value to null.
 */
inline void Value::setNull() {
  tagAsNull(); // This gets overwritten for DECIMAL -- but that's OK.
  switch (getValueType())
  {
    case VALUE_TYPE_BOOLEAN:
      // HACK BOOL NULL
      *reinterpret_cast<int8_t*>(m_data) = INT8_NULL;
      break;
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_INVALID:
      return;
    case VALUE_TYPE_TINYINT:
      getTinyInt() = INT8_NULL;
      break;
    case VALUE_TYPE_SMALLINT:
      getSmallInt() = INT16_NULL;
      break;
    case VALUE_TYPE_INTEGER:
      getInteger() = INT32_NULL;
      break;
    case VALUE_TYPE_TIMESTAMP:
      getTimestamp() = INT64_NULL;
      break;
    case VALUE_TYPE_BIGINT:
      getBigInt() = INT64_NULL;
      break;
    case VALUE_TYPE_DOUBLE:
      getDouble() = DOUBLE_MIN;
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
      *reinterpret_cast<void**>(m_data) = NULL;
      break;
    case VALUE_TYPE_DECIMAL:
      getDecimal().SetMin();
      break;
    default: {
      throw Exception("Value::setNull() called with unsupported ValueType " + std::to_string(getValueType()));
    }
  }
}

/**
 * Initialize an Value of the specified type from the tuple
 * storage area provided. If this is an Object type then the third
 * argument indicates whether the object is stored in the tuple inline.
 */
inline Value Value::initFromTupleStorage(const void *storage, ValueType type, bool isInlined)
{
  Value retval(type);
  switch (type)
  {
    case VALUE_TYPE_INTEGER:
      if ((retval.getInteger() = *reinterpret_cast<const int32_t*>(storage)) == INT32_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_BIGINT:
      if ((retval.getBigInt() = *reinterpret_cast<const int64_t*>(storage)) == INT64_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_DOUBLE:
      if ((retval.getDouble() = *reinterpret_cast<const double*>(storage)) <= DOUBLE_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    {
      //Potentially non-inlined type requires special handling
      if (isInlined) {
        //If it is inlined the storage area contains the actual data so copy a reference
        //to the storage area
        const char* inline_data = reinterpret_cast<const char*>(storage);
        *reinterpret_cast<const char**>(retval.m_data) = inline_data;
        retval.setSourceInlined(true);
        /**
         * If a string is inlined in its storage location there will be no pointer to
         * check for NULL. The length preceding value must be used instead.
         */
        if ((inline_data[0] & OBJECT_NULL_BIT) != 0) {
          retval.tagAsNull();
          break;
        }
        int length = inline_data[0];
        //std::cout << "Value::initFromTupleStorage: length: " << length << std::endl;
        retval.setObjectLength(length); // this unsets the null tag.
        break;
      }

      // If it isn't inlined the storage area contains a pointer to the
      // Varlen object containing the string's memory
      Varlen* sref = *reinterpret_cast<Varlen**>(const_cast<void*>(storage));
      *reinterpret_cast<Varlen**>(retval.m_data) = sref;
      // If the Varlen pointer is null, that's because this
      // was a null value; otherwise get the right char* from the Varlen
      if (sref == NULL) {
        retval.tagAsNull();
        break;
      }

      // Cache the object length in the Value.

      /* The format for a length preceding value is a 1-byte short representation
       * with the the 7th bit used to indicate a null value and the 8th bit used
       * to indicate that this is part of a long representation and that 3 bytes
       * follow. 6 bits are available to represent length for a maximum length
       * of 63 bytes representable with a single byte length. 30 bits are available
       * when the continuation bit is set and 3 bytes follow.
       *
       * The value is converted to network byte order so that the code
       * will always know which byte contains the most signficant digits.
       */

      /*
       * Generated mask that removes the null and continuation bits
       * from a single byte length value
       */
      const char mask = ~static_cast<char>(OBJECT_NULL_BIT | OBJECT_CONTINUATION_BIT);

      char* data = sref->Get();
      int32_t length = 0;
      if ((data[0] & OBJECT_CONTINUATION_BIT) != 0) {
        char numberBytes[4];
        numberBytes[0] = static_cast<char>(data[0] & mask);
        numberBytes[1] = data[1];
        numberBytes[2] = data[2];
        numberBytes[3] = data[3];
        length = ntohl(*reinterpret_cast<int32_t*>(numberBytes));
      } else {
        length = data[0] & mask;
      }

      //std::cout << "Value::initFromTupleStorage: length: " << length << std::endl;
      retval.setObjectLength(length); // this unsets the null tag.
      break;
    }
    case VALUE_TYPE_TIMESTAMP:
      if ((retval.getTimestamp() = *reinterpret_cast<const int64_t*>(storage)) == INT64_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_TINYINT:
      if ((retval.getTinyInt() = *reinterpret_cast<const int8_t*>(storage)) == INT8_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_SMALLINT:
      if ((retval.getSmallInt() = *reinterpret_cast<const int16_t*>(storage)) == INT16_NULL) {
        retval.tagAsNull();
      }
      break;
    case VALUE_TYPE_DECIMAL:
    {
      ::memcpy(retval.m_data, storage, sizeof(TTInt));
      break;
    }
    default:
      throw Exception(
          "Value::initFromTupleStorage() invalid column type " +
          ValueTypeToString(type));
      /* no break */
  }
  return retval;
}

/**
 * Serialize the scalar this Value represents to the provided
 * storage area. If the scalar is an Object type that is not
 * inlined then the provided data pool or the heap will be used to
 * allocated storage for a copy of the object.
 */
inline void Value::serializeToTupleStorageAllocateForObjects(void *storage, const bool isInlined,
                                                             const int32_t maxLength, const bool isInBytes, VarlenPool *dataPool) const
{
  const ValueType type = getValueType();

  switch (type) {
    case VALUE_TYPE_TIMESTAMP:
      *reinterpret_cast<int64_t*>(storage) = getTimestamp();
      break;
    case VALUE_TYPE_TINYINT:
      *reinterpret_cast<int8_t*>(storage) = getTinyInt();
      break;
    case VALUE_TYPE_SMALLINT:
      *reinterpret_cast<int16_t*>(storage) = getSmallInt();
      break;
    case VALUE_TYPE_INTEGER:
      *reinterpret_cast<int32_t*>(storage) = getInteger();
      break;
    case VALUE_TYPE_BIGINT:
      *reinterpret_cast<int64_t*>(storage) = getBigInt();
      break;
    case VALUE_TYPE_DOUBLE:
      *reinterpret_cast<double*>(storage) = getDouble();
      break;
    case VALUE_TYPE_DECIMAL:
      ::memcpy(storage, m_data, sizeof(TTInt));
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
      //Potentially non-inlined type requires special handling
      if (isInlined) {
        inlineCopyObject(storage, maxLength, isInBytes);
      }
      else {
        if (isNull()) {
          *reinterpret_cast<void**>(storage) = NULL;
        }
        else {
          int32_t objLength = getObjectLength_withoutNull();
          const char* ptr = reinterpret_cast<const char*>(getObjectValue_withoutNull());
          checkTooNarrowVarcharAndVarbinary(m_valueType, ptr, objLength, maxLength, isInBytes);

          const int8_t lengthLength = getObjectLengthLength();
          const int32_t minlength = lengthLength + objLength;
          Varlen* sref = Varlen::Create(minlength, dataPool);
          char *copy = sref->Get();
          setObjectLengthToLocation(objLength, copy);
          ::memcpy(copy + lengthLength, getObjectValue_withoutNull(), objLength);
          *reinterpret_cast<Varlen**>(storage) = sref;
        }
      }
      break;
    default: {
      throw Exception(
          "Value::serializeToTupleStorageAllocateForObjects() unrecognized type " +
          ValueTypeToString(type));
    }
  }
}

/**
 * Serialize the scalar this Value represents to the storage area
 * provided. If the scalar is an Object type then the object will be
 * copy if it can be inlined into the tuple. Otherwise a pointer to
 * the object will be copied into the storage area.  Any allocations
 * needed (if this Value refers to inlined memory whereas the field
 * in the tuple is not inlined), will be done in the temp string pool.
 */
inline void Value::serializeToTupleStorage(void *storage, const bool isInlined,
                                           const int32_t maxLength, const bool isInBytes) const
{
  const ValueType type = getValueType();
  switch (type) {
    case VALUE_TYPE_TIMESTAMP:
      *reinterpret_cast<int64_t*>(storage) = getTimestamp();
      break;
    case VALUE_TYPE_TINYINT:
      *reinterpret_cast<int8_t*>(storage) = getTinyInt();
      break;
    case VALUE_TYPE_SMALLINT:
      *reinterpret_cast<int16_t*>(storage) = getSmallInt();
      break;
    case VALUE_TYPE_INTEGER:
      *reinterpret_cast<int32_t*>(storage) = getInteger();
      break;
    case VALUE_TYPE_BIGINT:
      *reinterpret_cast<int64_t*>(storage) = getBigInt();
      break;
    case VALUE_TYPE_DOUBLE:
      *reinterpret_cast<double*>(storage) = getDouble();
      break;
    case VALUE_TYPE_DECIMAL:
      ::memcpy( storage, m_data, sizeof(TTInt));
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
      //Potentially non-inlined type requires special handling
      if (isInlined) {
        inlineCopyObject(storage, maxLength, isInBytes);
      }
      else {
        if (!isNull()) {
          int objLength = getObjectLength_withoutNull();
          const char* ptr = reinterpret_cast<const char*>(getObjectValue_withoutNull());
          checkTooNarrowVarcharAndVarbinary(m_valueType, ptr, objLength, maxLength, isInBytes);
        }

        // copy the Varlen pointers, even for NULL case.
        if (m_sourceInlined) {
          // create a non-const temp here for the outlined value
          Value outlinedValue = *this;
          outlinedValue.allocateObjectFromInlinedValue(getTempStringPool());
          *reinterpret_cast<Varlen**>(storage) =
              *reinterpret_cast<Varlen* const*>(outlinedValue.m_data);
        }
        else {
          *reinterpret_cast<Varlen**>(storage) = *reinterpret_cast<Varlen* const*>(m_data);
        }
      }
      break;
    default:
      char message[128];
      snprintf(message, 128, "Value::serializeToTupleStorage() unrecognized type '%s'",
               ValueTypeToString(type).c_str());
      throw Exception(message);
  }
}


/**
 * Deserialize a scalar value of the specified type from the
 * SerializeInput directly into the tuple storage area
 * provided. This function will perform memory allocations for
 * Object types as necessary using the provided data pool or the
 * heap. This is used to deserialize tables.
 */
inline void Value::deserializeFrom(SerializeInputBE &input, VarlenPool *dataPool, char *storage,
                                   const ValueType type, bool isInlined, int32_t maxLength, bool isInBytes) {
  deserializeFrom<TUPLE_SERIALIZATION_NATIVE>(input, dataPool, storage, type, isInlined, maxLength, isInBytes);
}

template <TupleSerializationFormat F, Endianess E> inline void Value::deserializeFrom(SerializeInput<E> &input, VarlenPool *dataPool,
                                                                                      char *storage,
                                                                                      const ValueType type, bool isInlined,
                                                                                      int32_t maxLength, bool isInBytes) {

  switch (type) {
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      *reinterpret_cast<int64_t*>(storage) = input.ReadLong();
      break;
    case VALUE_TYPE_TINYINT:
      *reinterpret_cast<int8_t*>(storage) = input.ReadByte();
      break;
    case VALUE_TYPE_SMALLINT:
      *reinterpret_cast<int16_t*>(storage) = input.ReadShort();
      break;
    case VALUE_TYPE_INTEGER:
      *reinterpret_cast<int32_t*>(storage) = input.ReadInt();
      break;
    case VALUE_TYPE_DOUBLE:
      *reinterpret_cast<double* >(storage) = input.ReadDouble();
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    {
      const int32_t length = input.ReadInt();

      const int8_t lengthLength = getAppropriateObjectLengthLength(length);
      // the NULL SQL string is a NULL C pointer
      if (isInlined) {
        // Always reset the bits regardless of how long the actual value is.
        ::memset(storage, 0, lengthLength + maxLength);

        setObjectLengthToLocation(length, storage);
        if (length == OBJECTLENGTH_NULL) {
          break;
        }
        const char *data = reinterpret_cast<const char*>(input.GetRawPointer(length));
        checkTooNarrowVarcharAndVarbinary(type, data, length, maxLength, isInBytes);

        ::memcpy( storage + lengthLength, data, length);
      } else {
        if (length == OBJECTLENGTH_NULL) {
          *reinterpret_cast<void**>(storage) = NULL;
          return;
        }
        const char *data = reinterpret_cast<const char*>(input.GetRawPointer(length));
        checkTooNarrowVarcharAndVarbinary(type, data, length, maxLength, isInBytes);

        const int32_t minlength = lengthLength + length;
        Varlen* sref = Varlen::Create(minlength, dataPool);
        char* copy = sref->Get();
        setObjectLengthToLocation( length, copy);
        ::memcpy(copy + lengthLength, data, length);
        *reinterpret_cast<Varlen**>(storage) = sref;
      }
      break;
    }
    case VALUE_TYPE_DECIMAL: {
      if (F == TUPLE_SERIALIZATION_DR) {
        const int scale = input.ReadByte();
        const int precisionBytes = input.ReadByte();
        if (scale != kMaxDecScale) {
          throw Exception("Unexpected scale %d", scale);
        }
        if (precisionBytes != 16) {
          throw Exception("Unexpected number of precision bytes %d", precisionBytes);
        }
      }
      uint64_t *longStorage = reinterpret_cast<uint64_t*>(storage);
      //Reverse order for Java BigDecimal BigEndian
      longStorage[1] = input.ReadLong();
      longStorage[0] = input.ReadLong();

      if (F == TUPLE_SERIALIZATION_DR) {
        // Serialize to export serializes them in network byte order, have to reverse them here
        longStorage[0] = ntohll(longStorage[0]);
        longStorage[1] = ntohll(longStorage[1]);
      }

      break;
    }
    default:
      char message[128];
      snprintf(message, 128, "Value::deserializeFrom() unrecognized type '%s'",
               ValueTypeToString(type).c_str());
      throw Exception(
          message);
  }
}

/**
 * Deserialize a scalar value of the specified type from the
 * provided SerializeInput and perform allocations as necessary.
 * This is used to deserialize parameter sets.
 */
inline void Value::deserializeFromAllocateForStorage(SerializeInputBE &input, VarlenPool *dataPool)
{
  const ValueType type = static_cast<ValueType>(input.ReadByte());
  deserializeFromAllocateForStorage(type, input, dataPool);
}

inline void Value::deserializeFromAllocateForStorage(ValueType type, SerializeInputBE &input, VarlenPool *dataPool)
{
  setValueType(type);
  // Parameter array Value elements are reused from one executor call to the next,
  // so these Values need to forget they were ever null.
  m_data[13] = 0; // effectively, this is tagAsNonNull()
  switch (type) {
    case VALUE_TYPE_BIGINT:
      getBigInt() = input.ReadLong();
      if (getBigInt() == INT64_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_TIMESTAMP:
      getTimestamp() = input.ReadLong();
      if (getTimestamp() == INT64_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_TINYINT:
      getTinyInt() = input.ReadByte();
      if (getTinyInt() == INT8_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_SMALLINT:
      getSmallInt() = input.ReadShort();
      if (getSmallInt() == INT16_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_INTEGER:
      getInteger() = input.ReadInt();
      if (getInteger() == INT32_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_DOUBLE:
      getDouble() = input.ReadDouble();
      if (getDouble() <= DOUBLE_NULL) {
        tagAsNull();
      }
      break;
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    {
      const int32_t length = input.ReadInt();
      // the NULL SQL string is a NULL C pointer
      if (length == OBJECTLENGTH_NULL) {
        setNull();
        break;
      }
      char* storage = allocateValueStorage(length, dataPool);
      const char *str = (const char*) input.GetRawPointer(length);
      ::memcpy(storage, str, length);
      break;
    }
    case VALUE_TYPE_DECIMAL: {
      getDecimal().table[1] = input.ReadLong();
      getDecimal().table[0] = input.ReadLong();
      break;
    }
    case VALUE_TYPE_NULL: {
      setNull();
      break;
    }
    case VALUE_TYPE_ARRAY: {
      deserializeIntoANewValueList(input, dataPool);
      break;
    }
    default:
      throw Exception("Value::deserializeFromAllocateForStorage() unrecognized type " +
                      ValueTypeToString(type));
  }
}

/**
 * Serialize this Value to the provided SerializeOutput
 */
inline void Value::serializeTo(SerializeOutput &output) const {
  const ValueType type = getValueType();
  switch (type) {
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    {
      if (isNull()) {
        output.WriteInt(OBJECTLENGTH_NULL);
        break;
      }
      const int32_t length = getObjectLength_withoutNull();
      if (length <= OBJECTLENGTH_NULL) {
        throw Exception("Attempted to serialize an Value with a negative length");
      }
      output.WriteInt(static_cast<int32_t>(length));

      // Not a null string: write it out
      output.WriteBytes(getObjectValue_withoutNull(), length);

      break;
    }
    case VALUE_TYPE_TINYINT: {
      output.WriteByte(getTinyInt());
      break;
    }
    case VALUE_TYPE_SMALLINT: {
      output.WriteShort(getSmallInt());
      break;
    }
    case VALUE_TYPE_INTEGER: {
      output.WriteInt(getInteger());
      break;
    }
    case VALUE_TYPE_TIMESTAMP: {
      output.WriteLong(getTimestamp());
      break;
    }
    case VALUE_TYPE_BIGINT: {
      output.WriteLong(getBigInt());
      break;
    }
    case VALUE_TYPE_DOUBLE: {
      output.WriteDouble(getDouble());
      break;
    }
    case VALUE_TYPE_DECIMAL: {
      output.WriteLong(getDecimal().table[1]);
      output.WriteLong(getDecimal().table[0]);
      break;
    }
    default:
      throw Exception( "Value::serializeTo() found a column "
          "with ValueType '%s' that is not handled" + getValueTypeString());
  }
}

inline void Value::serializeToExport_withoutNull(ExportSerializeOutput &io) const
{
  assert(isNull() == false);
  const ValueType type = getValueType();
  switch (type) {
    case VALUE_TYPE_VARCHAR:
    case VALUE_TYPE_VARBINARY:
    {
      io.WriteBinaryString(getObjectValue_withoutNull(), getObjectLength_withoutNull());
      return;
    }
    case VALUE_TYPE_TINYINT: {
      io.WriteByte(getTinyInt());
      return;
    }
    case VALUE_TYPE_SMALLINT: {
      io.WriteShort(getSmallInt());
      return;
    }
    case VALUE_TYPE_INTEGER: {
      io.WriteInt(getInteger());
      return;
    }
    case VALUE_TYPE_TIMESTAMP: {
      io.WriteLong(getTimestamp());
      return;
    }
    case VALUE_TYPE_BIGINT: {
      io.WriteLong(getBigInt());
      return;
    }
    case VALUE_TYPE_DOUBLE: {
      io.WriteDouble(getDouble());
      return;
    }
    case VALUE_TYPE_DECIMAL: {
      io.WriteByte((int8_t)kMaxDecScale);
      io.WriteByte((int8_t)16);  //number of bytes in decimal
      io.WriteLong(htonll(getDecimal().table[1]));
      io.WriteLong(htonll(getDecimal().table[0]));
      return;
    }
    case VALUE_TYPE_INVALID:
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOLEAN:
    case VALUE_TYPE_ADDRESS:
    case VALUE_TYPE_ARRAY:
    case VALUE_TYPE_FOR_DIAGNOSTICS_ONLY_NUMERIC:
      char message[128];
      snprintf(message, sizeof(message), "Invalid type in serializeToExport: %s", ValueTypeToString(getValueType()).c_str());
      throw Exception(message);
  }

  throw Exception("Invalid type in serializeToExport");
}

/** Reformat an object-typed value from its inlined form to its
 *  allocated out-of-line form, for use with a wider/widened tuple
 *  column.  Use the pool specified by the caller, or the temp string
 *  pool if none was supplied. **/
inline void Value::allocateObjectFromInlinedValue(VarlenPool* pool = NULL)
{
  if (m_valueType == VALUE_TYPE_NULL || m_valueType == VALUE_TYPE_INVALID) {
    return;
  }
  assert(m_valueType == VALUE_TYPE_VARCHAR || m_valueType == VALUE_TYPE_VARBINARY);
  assert(m_sourceInlined);

  if (isNull()) {
    *reinterpret_cast<void**>(m_data) = NULL;
    // serializeToTupleStorage fusses about this inline flag being set, even for NULLs
    setSourceInlined(false);
    return;
  }

  if (pool == NULL) {
    pool = getTempStringPool();
  }

  // When an object is inlined, m_data is a direct pointer into a tuple's inline storage area.
  char* source = *reinterpret_cast<char**>(m_data);

  // When it isn't inlined, m_data must contain a pointer to a Varlen object
  // that contains that same data in that same format.

  int32_t length = getObjectLength_withoutNull();
  // inlined objects always have a minimal (1-byte) length field.
  Varlen* sref = Varlen::Create(length + SHORT_OBJECT_LENGTHLENGTH, pool);
  char* storage = sref->Get();
  // Copy length and value into the allocated out-of-line storage
  ::memcpy(storage, source, length + SHORT_OBJECT_LENGTHLENGTH);
  setObjectValue(sref);
  setSourceInlined(false);
}

/** Deep copy an outline object-typed value from its current allocated pool,
 *  allocate the new outline object in the global temp string pool instead.
 *  The caller needs to deallocate the original outline space for the object,
 *  probably by purging the pool that contains it.
 *  This function is used in the aggregate function for MIN/MAX functions.
 *  **/
inline void Value::allocateObjectFromOutlinedValue()
{
  if (m_valueType == VALUE_TYPE_NULL || m_valueType == VALUE_TYPE_INVALID) {
    return;
  }
  assert(m_valueType == VALUE_TYPE_VARCHAR || m_valueType == VALUE_TYPE_VARBINARY);
  assert(!m_sourceInlined);

  if (isNull()) {
    *reinterpret_cast<void**>(m_data) = NULL;
    return;
  }
  VarlenPool* pool = getTempStringPool();

  // get the outline data
  const char* source = (*reinterpret_cast<Varlen* const*>(m_data))->Get();

  const int32_t length = getObjectLength_withoutNull() + getObjectLengthLength();
  Varlen* sref = Varlen::Create(length, pool);
  char* storage = sref->Get();
  // Copy the value into the allocated out-of-line storage
  ::memcpy(storage, source, length);
  setObjectValue(sref);
  setSourceInlined(false);
}

inline bool Value::isNull() const {
  if (getValueType() == VALUE_TYPE_DECIMAL) {
    TTInt min;
    min.SetMin();
    return getDecimal() == min;
  }
  return m_data[13] == OBJECT_NULL_BIT;
}

inline bool Value::isNaN() const {
  if (getValueType() == VALUE_TYPE_DOUBLE) {
    return std::isnan(getDouble());
  }
  return false;
}

// general full comparison
inline Value Value::op_equals(const Value rhs) const {
  return compare(rhs) == 0 ? getTrue() : getFalse();
}

inline Value Value::op_notEquals(const Value rhs) const {
  return compare(rhs) != 0 ? getTrue() : getFalse();
}

inline Value Value::op_lessThan(const Value rhs) const {
  return compare(rhs) < 0 ? getTrue() : getFalse();
}

inline Value Value::op_lessThanOrEqual(const Value rhs) const {
  return compare(rhs) <= 0 ? getTrue() : getFalse();
}

inline Value Value::op_greaterThan(const Value rhs) const {
  return compare(rhs) > 0 ? getTrue() : getFalse();
}

inline Value Value::op_greaterThanOrEqual(const Value rhs) const {
  return compare(rhs) >= 0 ? getTrue() : getFalse();
}

// without null comparison
inline Value Value::op_equals_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) == 0 ? getTrue() : getFalse();
}

inline Value Value::op_notEquals_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) != 0 ? getTrue() : getFalse();
}

inline Value Value::op_lessThan_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) < 0 ? getTrue() : getFalse();
}

inline Value Value::op_lessThanOrEqual_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) <= 0 ? getTrue() : getFalse();
}

inline Value Value::op_greaterThan_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) > 0 ? getTrue() : getFalse();
}

inline Value Value::op_greaterThanOrEqual_withoutNull(const Value rhs) const {
  return compare_withoutNull(rhs) >= 0 ? getTrue() : getFalse();
}

inline Value Value::op_max(const Value rhs) const {
  if (compare(rhs) > 0) {
    return *this;
  } else {
    return rhs;
  }
}

inline Value Value::op_min(const Value rhs) const {
  if (compare(rhs) < 0) {
    return *this;
  } else {
    return rhs;
  }
}

inline Value Value::getNullValue(ValueType type) {
  Value retval(type);
  retval.setNull();
  return retval;
}

inline void Value::hashCombine(std::size_t &seed) const {
  const ValueType type = getValueType();
  switch (type) {
    case VALUE_TYPE_TINYINT:
      boost::hash_combine( seed, getTinyInt()); break;
    case VALUE_TYPE_SMALLINT:
      boost::hash_combine( seed, getSmallInt()); break;
    case VALUE_TYPE_INTEGER:
      boost::hash_combine( seed, getInteger()); break;
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      boost::hash_combine( seed, getBigInt()); break;
    case VALUE_TYPE_DOUBLE:
      // This method was observed to fail on Centos 5 / GCC 4.1.2, returning different hashes
      // for identical inputs, so the conditional was added,
      // mutated from the one in boost/type_traits/intrinsics.hpp,
      // and the broken overload for "double" was by-passed in favor of the more reliable
      // one for int64 -- even if this may give sub-optimal hashes for typical collections of double.
      // This conditional can be dropped when Centos 5 support is dropped.
#if defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 2) && !defined(__GCCXML__))) && !defined(BOOST_CLANG)
      boost::hash_combine( seed, getDouble()); break;
#else
      {
        const int64_t proxyForDouble =  *reinterpret_cast<const int64_t*>(m_data);
        boost::hash_combine( seed, proxyForDouble); break;
      }
#endif
    case VALUE_TYPE_VARCHAR: {
      if (isNull()) {
        boost::hash_combine( seed, std::string(""));
      } else {
        const int32_t length = getObjectLength_withoutNull();
        boost::hash_combine( seed, std::string( reinterpret_cast<const char*>(getObjectValue_withoutNull()), length ));
      }
      break;
    }
    case VALUE_TYPE_VARBINARY: {
      if (isNull()) {
        boost::hash_combine( seed, std::string(""));
      } else {
        const int32_t length = getObjectLength_withoutNull();
        char* data = reinterpret_cast<char*>(getObjectValue_withoutNull());
        for (int32_t i = 0; i < length; i++)
          boost::hash_combine(seed, data[i]);
      }
      break;
    }
    case VALUE_TYPE_DECIMAL:
      getDecimal().hash(seed); break;
    default:
      throw Exception("Value::hashCombine unknown type " +  getValueTypeString());
  }
}


inline Value Value::castAs(ValueType type) const {
  LOG_TRACE("Converting from %s to %s",
            ValueTypeToString(getValueType()).c_str(),
            ValueTypeToString(type).c_str());
  if (getValueType() == type) {
    return *this;
  }
  if (isNull()) {
    return getNullValue(type);
  }

  switch (type) {
    case VALUE_TYPE_TINYINT:
      return castAsTinyInt();
    case VALUE_TYPE_SMALLINT:
      return castAsSmallInt();
    case VALUE_TYPE_INTEGER:
      return castAsInteger();
    case VALUE_TYPE_BIGINT:
      return castAsBigInt();
    case VALUE_TYPE_TIMESTAMP:
      return castAsTimestamp();
    case VALUE_TYPE_DOUBLE:
      return castAsDouble();
    case VALUE_TYPE_VARCHAR:
      return castAsString();
    case VALUE_TYPE_VARBINARY:
      return castAsBinary();
    case VALUE_TYPE_DECIMAL:
      return castAsDecimal();
    default:
      char message[128];
      snprintf(message, 128, "Type %d not a recognized type for casting",
               (int) type);
      throw Exception(message);
  }
}

inline void* Value::castAsAddress() const {
  const ValueType type = getValueType();
  switch (type) {
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_ADDRESS:
      return *reinterpret_cast<void* const*>(m_data);
    default:
      throw Exception(
          "Type %s not a recognized type for casting as an address" + getValueTypeString());
  }
}

inline Value Value::op_increment() const {
  const ValueType type = getValueType();
  Value retval(type);
  switch(type) {
    case VALUE_TYPE_TINYINT:
      if (getTinyInt() == INT8_MAX) {
        throw Exception(
            "Incrementing this TinyInt results in a value out of range");
      }
      retval.getTinyInt() = static_cast<int8_t>(getTinyInt() + 1); break;
    case VALUE_TYPE_SMALLINT:
      if (getSmallInt() == INT16_MAX) {
        throw Exception(
            "Incrementing this SmallInt results in a value out of range");
      }
      retval.getSmallInt() = static_cast<int16_t>(getSmallInt() + 1); break;
    case VALUE_TYPE_INTEGER:
      if (getInteger() == INT32_MAX) {
        throw Exception(
            "Incrementing this Integer results in a value out of range");
      }
      retval.getInteger() = getInteger() + 1; break;
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      if (getBigInt() == INT64_MAX) {
        throw Exception(
            "Incrementing this BigInt/Timestamp results in a value out of range");
      }
      retval.getBigInt() = getBigInt() + 1; break;
    case VALUE_TYPE_DOUBLE:
      retval.getDouble() = getDouble() + 1; break;
    default:
      throw Exception( "type %s is not incrementable " + getValueTypeString());
      break;
  }
  return retval;
}

inline Value Value::op_decrement() const {
  const ValueType type = getValueType();
  Value retval(type);
  switch(type) {
    case VALUE_TYPE_TINYINT:
      if (getTinyInt() == PELOTON_INT8_MIN) {
        throw Exception(
            "Decrementing this TinyInt results in a value out of range");
      }
      retval.getTinyInt() = static_cast<int8_t>(getTinyInt() - 1); break;
    case VALUE_TYPE_SMALLINT:
      if (getSmallInt() == PELOTON_INT16_MIN) {
        throw Exception(
            "Decrementing this SmallInt results in a value out of range");
      }
      retval.getSmallInt() = static_cast<int16_t>(getSmallInt() - 1); break;
    case VALUE_TYPE_INTEGER:
      if (getInteger() == PELOTON_INT32_MIN) {
        throw Exception(
            "Decrementing this Integer results in a value out of range");
      }
      retval.getInteger() = getInteger() - 1; break;
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      if (getBigInt() == PELOTON_INT64_MIN) {
        throw Exception(
            "Decrementing this BigInt/Timestamp results in a value out of range");
      }
      retval.getBigInt() = getBigInt() - 1; break;
    case VALUE_TYPE_DOUBLE:
      retval.getDouble() = getDouble() - 1; break;
    default:
      throw Exception( "type %s is not decrementable " + getValueTypeString());
      break;
  }
  return retval;
}

inline bool Value::isZero() const {
  const ValueType type = getValueType();
  switch(type) {
    case VALUE_TYPE_TINYINT:
      return getTinyInt() == 0;
    case VALUE_TYPE_SMALLINT:
      return getSmallInt() == 0;
    case VALUE_TYPE_INTEGER:
      return getInteger() == 0;
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return getBigInt() == 0;
    case VALUE_TYPE_DECIMAL:
      return getDecimal().IsZero();
    default:
      throw Exception(
          "type %s is not a numeric type that implements isZero()" +
          getValueTypeString());
  }
}

inline Value Value::op_subtract(const Value rhs) const {
  ValueType vt = promoteForOp(getValueType(), rhs.getValueType());
  if (isNull() || rhs.isNull()) {
    return getNullValue(vt);
  }

  switch (vt) {
    case VALUE_TYPE_TINYINT:
    case VALUE_TYPE_SMALLINT:
    case VALUE_TYPE_INTEGER:
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return opSubtractBigInts(castAsBigIntAndGetValue(),
                               rhs.castAsBigIntAndGetValue());

    case VALUE_TYPE_DOUBLE:
      return opSubtractDoubles(castAsDoubleAndGetValue(),
                               rhs.castAsDoubleAndGetValue());

    case VALUE_TYPE_DECIMAL:
      return opSubtractDecimals(castAsDecimal(),
                                rhs.castAsDecimal());

    default:
      break;
  }
  throw TypeMismatchException("Promotion of %s and %s failed in op_subtract.",
                              getValueType(),
                              rhs.getValueType());
}

inline Value Value::op_add(const Value rhs) const {
  ValueType vt = promoteForOp(getValueType(), rhs.getValueType());
  if (isNull() || rhs.isNull()) {
    return getNullValue(vt);
  }

  switch (vt) {
    case VALUE_TYPE_TINYINT:
    case VALUE_TYPE_SMALLINT:
    case VALUE_TYPE_INTEGER:
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return opAddBigInts(castAsBigIntAndGetValue(),
                          rhs.castAsBigIntAndGetValue());

    case VALUE_TYPE_DOUBLE:
      return opAddDoubles(castAsDoubleAndGetValue(),
                          rhs.castAsDoubleAndGetValue());

    case VALUE_TYPE_DECIMAL:
      return opAddDecimals(castAsDecimal(),
                           rhs.castAsDecimal());

    default:
      break;
  }
  throw Exception("Promotion of %s and %s failed in op_add." +
                  getValueTypeString() +
                  rhs.getValueTypeString());
}

inline Value Value::op_multiply(const Value rhs) const {
  ValueType vt = promoteForOp(getValueType(), rhs.getValueType());
  if (isNull() || rhs.isNull()) {
    return getNullValue(vt);
  }

  switch (vt) {
    case VALUE_TYPE_TINYINT:
    case VALUE_TYPE_SMALLINT:
    case VALUE_TYPE_INTEGER:
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return opMultiplyBigInts(castAsBigIntAndGetValue(),
                               rhs.castAsBigIntAndGetValue());

    case VALUE_TYPE_DOUBLE:
      return opMultiplyDoubles(castAsDoubleAndGetValue(),
                               rhs.castAsDoubleAndGetValue());

    case VALUE_TYPE_DECIMAL:
      return opMultiplyDecimals(castAsDecimal(),
                                rhs.castAsDecimal());

    default:
      break;
  }
  throw Exception("Promotion of %s and %s failed in op_multiply." +
                  getValueTypeString() +
                  rhs.getValueTypeString());
}

inline Value Value::op_divide(const Value rhs) const {
  ValueType vt = promoteForOp(getValueType(), rhs.getValueType());
  if (isNull() || rhs.isNull()) {
    return getNullValue(vt);
  }

  switch (vt) {
    case VALUE_TYPE_TINYINT:
    case VALUE_TYPE_SMALLINT:
    case VALUE_TYPE_INTEGER:
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_TIMESTAMP:
      return opDivideBigInts(castAsBigIntAndGetValue(),
                             rhs.castAsBigIntAndGetValue());

    case VALUE_TYPE_DOUBLE:
      return opDivideDoubles(castAsDoubleAndGetValue(),
                             rhs.castAsDoubleAndGetValue());

    case VALUE_TYPE_DECIMAL:
      return opDivideDecimals(castAsDecimal(),
                              rhs.castAsDecimal());

    default:
      break;
  }
  throw Exception("Promotion of %s and %s failed in op_divide." +
                  getValueTypeString() +
                  rhs.getValueTypeString());
}

/*
 * Out must have storage for 16 bytes
 */
inline int32_t Value::murmurHash3() const {
  const ValueType type = getValueType();
  switch(type) {
    case VALUE_TYPE_TIMESTAMP:
    case VALUE_TYPE_DOUBLE:
    case VALUE_TYPE_BIGINT:
    case VALUE_TYPE_INTEGER:
    case VALUE_TYPE_SMALLINT:
    case VALUE_TYPE_TINYINT:
      return MurmurHash3_x64_128( m_data, 8, 0);
    case VALUE_TYPE_VARBINARY:
    case VALUE_TYPE_VARCHAR:
      if (isNull()) {
        // Use NULL check first to be able to get rid of checks inside of other functions.
        // Maybe it is impossible to be null here. -xin
        throw Exception("Must not ask  for object length on sql null object.");
      }
      return MurmurHash3_x64_128( getObjectValue_withoutNull(), getObjectLength_withoutNull(), 0);
    default:
      throw Exception("Unknown type for murmur hashing %d" + std::to_string(type));
      break;
  }
}

/*
 * The LHS (this) should always be the string being compared
 * and the RHS should always be the LIKE expression.
 * The planner or EE needs to enforce this.
 *
 * Null check should have been handled already.
 */
inline Value Value::like(const Value rhs) const {
  /*
   * Validate that all params are VARCHAR
   */
  const ValueType mType = getValueType();
  if (mType != VALUE_TYPE_VARCHAR) {
    throw Exception(
        "lhs of LIKE expression is %s not %s" +
        getValueTypeString() +
        ValueTypeToString(VALUE_TYPE_VARCHAR));
  }

  const ValueType rhsType = rhs.getValueType();
  if (rhsType != VALUE_TYPE_VARCHAR) {
    throw Exception(
        "rhs of LIKE expression is %s not %s" +
        rhs.getValueTypeString() +
        ValueTypeToString(VALUE_TYPE_VARCHAR));
  }

  const int32_t valueUTF8Length = getObjectLength_withoutNull();
  const int32_t patternUTF8Length = rhs.getObjectLength_withoutNull();

  if (0 == patternUTF8Length) {
    if (0 == valueUTF8Length) {
      return getTrue();
    } else {
      return getFalse();
    }
  }

  char *valueChars = reinterpret_cast<char*>(getObjectValue_withoutNull());
  char *patternChars = reinterpret_cast<char*>(rhs.getObjectValue_withoutNull());
  assert(valueChars);
  assert(patternChars);

  /*
   * Because lambdas are for poseurs.
   */
  class Liker {

   private:
    // Constructor used internally for temporary recursion contexts.
    Liker( const Liker& original, const char* valueChars, const char* patternChars) :
      m_value(original.m_value, valueChars),
      m_pattern(original.m_pattern, patternChars)
   {}

   public:
    Liker(char *valueChars, char* patternChars, int32_t valueUTF8Length, int32_t patternUTF8Length) :
      m_value(valueChars, valueChars + valueUTF8Length),
      m_pattern(patternChars, patternChars + patternUTF8Length)
   {}

    bool like() {
      while ( ! m_pattern.atEnd()) {
        const uint32_t nextPatternCodePoint = m_pattern.extractCodePoint();
        switch (nextPatternCodePoint) {
          case '%': {
            if (m_pattern.atEnd()) {
              return true;
            }

            const char *postPercentPatternIterator = m_pattern.getCursor();
            const uint32_t nextPatternCodePointAfterPercent = m_pattern.extractCodePoint();
            const bool nextPatternCodePointAfterPercentIsSpecial =
                (nextPatternCodePointAfterPercent == '_') ||
                (nextPatternCodePointAfterPercent == '%');

            /*
             * This loop tries to skip as many characters as possible with the % by checking
             * if the next value character matches the pattern character after the %.
             *
             * If the next pattern character is special then we always have to recurse to
             * match that character. For stacked %s this just skips to the last one.
             * For stacked _ it will recurse and demand the correct number of characters.
             *
             * For a regular character it will recurse if the value character matches the pattern character.
             * This saves doing a function call per character and allows us to skip if there is no match.
             */
            while ( ! m_value.atEnd()) {

              const char *preExtractionValueIterator = m_value.getCursor();
              const uint32_t nextValueCodePoint = m_value.extractCodePoint();

              const bool nextPatternCodePointIsSpecialOrItEqualsNextValueCodePoint =
                  (nextPatternCodePointAfterPercentIsSpecial ||
                      (nextPatternCodePointAfterPercent == nextValueCodePoint));

              if ( nextPatternCodePointIsSpecialOrItEqualsNextValueCodePoint) {
                Liker recursionContext( *this, preExtractionValueIterator, postPercentPatternIterator);
                if (recursionContext.like()) {
                  return true;
                }
              }
            }
            return false;
          }
          case '_': {
            if ( m_value.atEnd()) {
              return false;
            }
            //Extract a code point to consume a character
            m_value.extractCodePoint();
            break;
          }
          default: {
            if ( m_value.atEnd()) {
              return false;
            }
            const uint32_t nextValueCodePoint = m_value.extractCodePoint();
            if (nextPatternCodePoint != nextValueCodePoint) {
              return false;
            }
            break;
          }
        }
      }
      //A matching value ends exactly where the pattern ends (having already accounted for '%')
      return m_value.atEnd();
    }

    UTF8Iterator m_value;
    UTF8Iterator m_pattern;
  };

  Liker liker(valueChars, patternChars, valueUTF8Length, patternUTF8Length);

  return liker.like() ? getTrue() : getFalse();
}

}  // End peloton namespace
