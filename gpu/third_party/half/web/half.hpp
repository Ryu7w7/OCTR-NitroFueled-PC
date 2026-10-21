// half - IEEE 754-based half-precision floating point library.
//
// Copyright (c) 2012-2017 Christian Rau <rauy@users.sourceforge.net>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation 
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, 
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE 
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR 
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// This is a dummy header file to provide a clean interface documentation to Doxygen, without any ugly internals.

/// \file
/// Main header file of the library.


/// Default rounding mode.
/// This specifies the rounding mode used for all conversions between [half](\ref half_float::half)s and `float`s as well as 
/// for the half_cast() if not specifying a rounding mode explicitly. It can be redefined (before including `half.hpp`) to one 
/// of the standard rounding modes using their respective constants or the equivalent values of 
/// [std::float_round_style](http://en.cppreference.com/w/cpp/types/numeric_limits/float_round_style):
///
/// `std::float_round_style`         | value | rounding
/// ---------------------------------|-------|-------------------------
/// `std::round_indeterminate`       | -1    | fastest (default)
/// `std::round_toward_zero`         | 0     | toward zero
/// `std::round_to_nearest`          | 1     | to nearest
/// `std::round_toward_infinity`     | 2     | toward positive infinity
/// `std::round_toward_neg_infinity` | 3     | toward negative infinity
///
/// By default this is set to `-1` (`std::round_indeterminate`), which uses truncation (round toward zero, but with overflows 
/// set to infinity) and is the fastest rounding mode possible. It can even be set to 
/// [std::numeric_limits<float>::round_style](http://en.cppreference.com/w/cpp/types/numeric_limits/round_style) to synchronize 
/// the rounding mode with that of the underlying single-precision implementation.
#define HALF_ROUND_STYLE	-1

/// Tie-breaking behaviour for round to nearest.
/// This specifies if ties in round to nearest should be resolved by rounding to the nearest even value. By default this is 
/// defined to `0` resulting in the faster but slightly more biased behaviour of rounding away from zero in half-way cases (and 
/// thus equal to the round() function), but can be redefined to `1` (before including half.hpp) if more IEEE-conformant 
/// behaviour is needed.
#define HALF_ROUND_TIES_TO_EVEN	0

/// Value signaling overflow.
/// In correspondence with `HUGE_VAL[F|L]` from `<cmath>` this symbol expands to a positive value signaling the overflow of an 
/// operation, in particular it just evaluates to positive infinity.
///
/// **See also:** Documentation for [HUGE_VAL](http://en.cppreference.com/w/cpp/numeric/math/HUGE_VAL)
#define HUGE_VALH		std::numeric_limits<half_float::half>::infinity()

/// Fast half-precision fma function.
/// This symbol is only defined if the fma() function generally executes as fast as, or faster than, a separate 
/// half-precision multiplication followed by an addition. Due to the internal single-precision implementation of all 
/// arithmetic operations, this is in fact always the case.
///
/// **See also:** Documentation for [FP_FAST_FMA](http://en.cppreference.com/w/cpp/numeric/math/fma)
#define FP_FAST_FMAH	1


/// Main namespace for half precision functionality.
/// This namespace contains all the functionality provided by the library.
namespace half_float
{
	/// Half-precision floating point type.
	/// This class implements an IEEE-conformant half-precision floating point type with the usual arithmetic operators and 
	/// conversions. It is implicitly convertible to single-precision floating point, which makes artihmetic expressions and 
	/// functions with mixed-type operands to be of the most precise operand type. Additionally all arithmetic operations 
	/// (and many mathematical functions) are carried out in single-precision internally. All conversions from single- to 
	/// half-precision are done using the library's default rounding mode, but temporary results inside chained arithmetic 
	/// expressions are kept in single-precision as long as possible (while of course still maintaining a strong half-precision type).
	///
	/// According to the C++98/03 definition, the half type is not a POD type. But according to C++11's less strict and 
	/// extended definitions it is both a standard layout type and a trivially copyable type (even if not a POD type), which 
	/// means it can be standard-conformantly copied using raw binary copies. But in this context some more words about the 
	/// actual size of the type. Although the half is representing an IEEE 16-bit type, it does not neccessarily have to be of 
	/// exactly 16-bits size. But on any reasonable implementation the actual binary representation of this type will most 
	/// probably not ivolve any additional "magic" or padding beyond the simple binary representation of the underlying 16-bit 
	/// IEEE number, even if not strictly guaranteed by the standard. But even then it only has an actual size of 16 bits if 
	/// your C++ implementation supports an unsigned integer type of exactly 16 bits width. But this should be the case on 
	/// nearly any reasonable platform.
	///
	/// So if your C++ implementation is not totally exotic or imposes special alignment requirements, it is a reasonable 
	/// assumption that the data of a half is just comprised of the 2 bytes of the underlying IEEE representation.
	struct half
	{
		/// \name Construction and assignment
		/// \{

		/// Default constructor.
		/// This initializes the half to 0. Although this does not match the builtin types' default-initialization semantics 
		/// and may be less efficient than no initialization, it is needed to provide proper value-initialization semantics.
		constexpr half() noexcept;

		/// Conversion constructor.
		/// \param rhs float to convert
		explicit half(float rhs);

		/// Conversion to single-precision.
		/// \return single precision value
		operator float() const;

		/// Single-precision assignment.
		/// \param rhs single-precision value to copy from
		/// \return reference to this half
		half& operator=(float rhs);

		/// \}
		/// \name Arithmetic updates
		/// \{

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \tparam E type of concrete half expression
		/// \param rhs half to add
		/// \return reference to this half
		half& operator+=(half rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \tparam E type of concrete half expression
		/// \param rhs half to subtract
		/// \return reference to this half
		half& operator-=(half rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \tparam E type of concrete half expression
		/// \param rhs half to multiply with
		/// \return reference to this half
		half& operator*=(half rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \tparam E type of concrete half expression
		/// \param rhs half to divide by
		/// \return reference to this half
		half& operator/=(half rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \param rhs single-precision value to add
		/// \return reference to this half
		half& operator+=(float rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \param rhs single-precision value to subtract
		/// \return reference to this half
		half& operator-=(float rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \param rhs single-precision value to multiply with
		/// \return reference to this half
		half& operator*=(float rhs);

		/// Arithmetic assignment.
		/// This operation uses the underlying single-precision implementation.
		/// \param rhs single-precision value to divide by
		/// \return reference to this half
		half& operator/=(float rhs);

		/// \}
		/// \name Increment and decrement
		/// \{

		/// Prefix increment.
		/// This operation uses the underlying single-precision implementation.
		/// \return incremented half value
		half& operator++();

		/// Prefix decrement.
		/// This operation uses the underlying single-precision implementation.
		/// \return decremented half value
		half& operator--();

		/// Postfix increment.
		/// This operation uses the underlying single-precision implementation.
		/// \return non-incremented half value
		half operator++(int);

		/// Postfix decrement.
		/// This operation uses the underlying single-precision implementation.
		/// \return non-decremented half value
		half operator--(int);
		/// \}
	};


	/// \name Comparison operators
	/// \{

	/// Comparison for equality.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if operands equal
	/// \retval false else
	bool operator==(half a, half b);

	/// Comparison for inequality.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if operands not equal
	/// \retval false else
	bool operator!=(half a, half b);

	/// Comparison for less than.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if \a a less than \a b
	/// \retval false else
	bool operator<(half a, half b);

	/// Comparison for greater than.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if \a a greater than \a b
	/// \retval false else
	bool operator>(half a, half b);

	/// Comparison for less equal.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if \a a less equal \a b
	/// \retval false else
	bool operator<=(half a, half b);

	/// Comparison for greater equal.
	/// \param a first operand
	/// \param b second operand
	/// \retval true if \a a greater equal \a b
	/// \retval false else
	bool operator>=(half a, half b);

	/// \}
	/// \name Arithmetic operators
	/// \{

	/// Add halfs.
	/// This operation uses the underlying single-precision implementation.
	/// \param a left operand
	/// \param b right operand
	/// \return sum of halfs
	half operator+(half a, half b);

	/// Subtract halfs.
	/// This operation uses the underlying single-precision implementation.
	/// \param a left operand
	/// \param b right operand
	/// \return difference of halfs
	half operator-(half a, half b);

	/// Multiply halfs.
	/// This operation uses the underlying single-precision implementation.
	/// \param a left operand
	/// \param b right operand
	/// \return product of halfs
	half operator*(half a, half b);

	/// Divide halfs.
	/// This operation uses the underlying single-precision implementation.
	/// \param a left operand
	/// \param b right operand
	/// \return quotient of halfs
	half operator/(half a, half b);

	/// Identity.
	/// \param h operand
	/// \return uncahnged operand
	half constexpr operator+(half h);

	/// Negation.
	/// \param h operand
	/// \return negated operand
	half constexpr operator-(half h);

	/// \}
	/// \name Input and output
	/// \{

	/// Output operator.
	/// \tparam charT character type
	/// \tparam traits character traits
	/// \param out output stream to write into
	/// \param h half to write
	/// \return reference to output stream
	template<typename charT,typename traits> std::basic_ostream<charT,traits>& operator<<(std::basic_ostream<charT,traits> &out, half h);

	/// Input operator.
	/// \tparam charT character type
	/// \tparam traits character traits
	/// \param in input stream to read from
	/// \param h half to read into
	/// \return reference to input stream
	template<typename charT,typename traits> std::basic_istream<charT,traits>& operator>>(std::basic_istream<charT,traits> &in, half &h);

	/// \}
	/// \name Basic mathematical operations
	/// \{

	/// Absolute value.
	/// \param arg operand
	/// \return absolute value of \a arg
	half abs(half arg);

	/// Absolute value.
	/// \param arg operand
	/// \return absolute value of \a arg
	half fabs(half arg);

	/// Remainder of division.
	/// This function uses the underlying single-precision implementation.
	/// \param x first operand
	/// \param y second operand
	/// \return remainder of floating point division.
	half fmod(half x, half y);

	/// Remainder of division.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param x first operand
	/// \param y second operand
	/// \return remainder of floating point division.
	half remainder(half x, half y);

	/// Remainder of division.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param x first operand
	/// \param y second operand
	/// \param quo address to store some bits of quotient at
	/// \return remainder of floating point division.
	half remquo(half x, half y, int *quo);

	/// Fused multiply add.
	/// This function uses the underlying single-precision implementation from C++11 `<cmath>` if it is supported **and** 
	/// faster than the straight-forward single-precision implementation (thus if `FP_FAST_FMAF` is defined).
	/// \param x first operand
	/// \param y second operand
	/// \param z third operand
	/// \return ( \a x * \a y ) + \a z rounded as one operation.
	half fma(half x, half y, half z);

	/// Minimum of halfs.
	/// \param x first operand
	/// \param y second operand
	/// \return minimum of operands
	half fmin(half x, half y);

	/// Maximum of halfs.
	/// \param x first operand
	/// \param y second operand
	/// \return maximum of operands
	half fmax(half x, half y);

	/// Positive difference.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param x first operand
	/// \param y second operand
	/// \return \a x - \a y or 0 if difference negative
	half fdim(half x, half y);

	/// Get NaN value.
	/// \return quiet NaN
	half nanh(const char*);

	/// \}
	/// \name Exponential functions
	/// \{

	/// Exponential function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return e raised to \a arg
	half exp(half arg);

	/// Binary exponential.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return 2 raised to \a arg
	half exp2(half arg);

	/// Exponential minus one.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return e raised to \a arg and subtracted by 1
	half expm1(half arg);

	/// Natural logorithm.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return logarithm of \a arg to base e
	half log(half arg);

	/// Common logorithm.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return logarithm of \a arg to base 10
	half log10(half arg);

	/// Natural logorithm.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return logarithm of \a arg + 1 to base e
	half log1p(half arg);

	/// Binary logorithm.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return logarithm of \a arg to base 2
	half log2(half arg);

	/// \}
	/// \name Power functions
	/// \{

	/// Square root.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return square root of \a arg
	half sqrt(half arg);

	/// Cubic root.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return cubic root of \a arg
	half cbrt(half arg);

	/// Hypotenuse function.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param x first argument
	/// \param y second argument
	/// \return square root of sum of squares without internal over- or underflows
	half hypot(half x, half y);

	/// Power function.
	/// This function uses the underlying single-precision implementation.
	/// \param base first argument
	/// \param exp second argument
	/// \return \a base raised to \a exp
	half pow(half base, half exp);

	/// \}
	/// \name Trigonometric functions
	/// \{

	/// Sine function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return sine value of \a arg
	half sin(half arg);

	/// Cosine function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return cosine value of \a arg
	half cos(half arg);

	/// Tangent function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return tangent value of \a arg
	half tan(half arg);

	/// Arc sine.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return arc sine value of \a arg
	half asin(half arg);

	/// Arc cosine function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return arc cosine value of \a arg
	half acos(half arg);

	/// Arc tangent function.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return arc tangent value of \a arg
	half atan(half arg);

	/// Arc tangent function.
	/// This function uses the underlying single-precision implementation.
	/// \param x first argument
	/// \param y second argument
	/// \return arc tangent value
	half atan2(half x, half y);

	/// \}
	/// \name Hyperbolic functions
	/// \{

	/// Hyperbolic sine.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return hyperbolic sine value of \a arg
	half sinh(half arg);

	/// Hyperbolic cosine.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return hyperbolic cosine value of \a arg
	half cosh(half arg);

	/// Hyperbolic tangent.
	/// This function uses the underlying single-precision implementation.
	/// \param arg function argument
	/// \return hyperbolic tangent value of \a arg
	half tanh(half arg);

	/// Hyperbolic area sine.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return hyperbolic area sine value of \a arg
	half asinh(half arg);

	/// Hyperbolic area cosine.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return hyperbolic area cosine value of \a arg
	half acosh(half arg);

	/// Hyperbolic area tangent.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return hyperbolic area tangent value of \a arg
	half atanh(half arg);

	/// \}
	/// \name Error and gamma functions
	/// \{

	/// Error function.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return error function value of \a arg
	half erf(half arg);

	/// Complementary error function.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return 1 minus error function value of \a arg
	half erfc(half arg);

	/// Natural logarithm of gamma function.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return natural logarith of gamma function for \a arg
	half lgamma(half arg);

	/// Gamma function.
	/// This function uses the underlying single-precision implementation if C++11 `<cmath>` functions are supported.
	/// \param arg function argument
	/// \return gamma function value of \a arg
	half tgamma(half arg);

	/// \}
	/// \name Rounding
	/// \{

	/// Nearest integer not less than half value.
	/// \param arg half to round
	/// \return nearest integer not less than \a arg
	half ceil(half arg);

	/// Nearest integer not greater than half value.
	/// \param arg half to round
	/// \return nearest integer not greater than \a arg
	half floor(half arg);

	/// Nearest integer not greater in magnitude than half value.
	/// \param arg half to round
	/// \return nearest integer not greater in magnitude than \a arg
	half trunc(half arg);

	/// Nearest integer.
	/// \param arg half to round
	/// \return nearest integer, rounded away from zero in half-way cases
	half round(half arg);

	/// Nearest integer.
	/// \param arg half to round
	/// \return nearest integer, rounded away from zero in half-way cases
	long lround(half arg);

	/// Nearest integer.
	/// This function requires support for C++11 `long long`.
	/// \param arg half to round
	/// \return nearest integer, rounded away from zero in half-way cases
	long long llround(half arg);

	/// Nearest integer.
	/// \tparam E type of half expression
	/// \param arg half expression to round
	/// \return nearest integer using default rounding mode
	half nearbyint(half arg);

	/// Nearest integer.
	/// \tparam E type of half expression
	/// \param arg half expression to round
	/// \return nearest integer using default rounding mode
	half rint(half arg);

	/// Nearest integer.
	/// \tparam E type of half expression
	/// \param arg half expression to round
	/// \return nearest integer using default rounding mode
	long lrint(half arg);

	/// Nearest integer.
	/// This function requires support for C++11 `long long`.
	/// \tparam E type of half expression
	/// \param arg half expression to round
	/// \return nearest integer using default rounding mode
	long long llrint(half arg);

	/// \}
	/// \name Floating point manipulation
	/// \{

	/// Decompress floating point number.
	/// \param arg number to decompress
	/// \param exp address to store exponent at
	/// \return significant in range [0.5, 1)
	half frexp(half arg, int *exp);

	/// Multiply by power of two.
	/// \param arg number to modify
	/// \param exp power of two to multiply with
	/// \return \a arg multplied by 2 raised to \a exp
	half ldexp(half arg, int exp);

	/// Extract integer and fractional parts.
	/// \param x number to decompress
	/// \param iptr address to store integer part at
	/// \return fractional part
	half modf(half x, half *iptr);

	/// Multiply by power of two.
	/// \param x number to modify
	/// \param exp power of two to multiply with
	/// \return \a arg multplied by 2 raised to \a exp
	half scalbn(half x, int exp);

	/// Multiply by power of two.
	/// \param x number to modify
	/// \param exp power of two to multiply with
	/// \return \a arg multplied by 2 raised to \a exp	
	half scalbln(half x, long exp);

	/// Extract exponent.
	/// \param arg number to query
	/// \return floating point exponent
	/// \retval FP_ILOGB0 for zero
	/// \retval FP_ILOGBNAN for NaN
	/// \retval MAX_INT for infinity
	int ilogb(half arg);

	/// Extract exponent.
	/// \param arg number to query
	/// \return floating point exponent
	half logb(half arg);

	/// Next representable value.
	/// \param from value to compute next representable value for
	/// \param to direction towards which to compute next value
	/// \return next representable value after \a from in direction towards \a to
	half nextafter(half from, half to);

	/// Next representable value.
	/// \param from value to compute next representable value for
	/// \param to direction towards which to compute next value
	/// \return next representable value after \a from in direction towards \a to
	half nexttoward(half from, long double to);

	/// Take sign.
	/// \param x value to change sign for
	/// \param y value to take sign from
	/// \return value equal to \a x in magnitude and to \a y in sign
	half copysign(half x, half y);

	/// \}
	/// \name Floating point classification
	/// \{

	/// Classify floating point value.
	/// \param arg number to classify
	/// \retval FP_ZERO for positive and negative zero
	/// \retval FP_SUBNORMAL for subnormal numbers
	/// \retval FP_INFINITY for positive and negative infinity
	/// \retval FP_NAN for NaNs
	/// \retval FP_NORMAL for all other (normal) values
	int fpclassify(half arg);

	/// Check if finite number.
	/// \param arg number to check
	/// \retval true if neither infinity nor NaN
	/// \retval false else
	bool isfinite(half arg);

	/// Check for infinity.
	/// \param arg number to check
	/// \retval true for positive or negative infinity
	/// \retval false else
	bool isinf(half arg);

	/// Check for NaN.
	/// \param arg number to check
	/// \retval true for NaNs
	/// \retval false else
	bool isnan(half arg);

	/// Check if normal number.
	/// \param arg number to check
	/// \retval true if normal number
	/// \retval false if either subnormal, zero, infinity or NaN
	bool isnormal(half arg);

	/// Check sign.
	/// \param arg number to check
	/// \retval true for negative number
	/// \retval false for positive number
	bool signbit(half arg);

	/// \}
	/// \name Comparison
	/// \{

	/// Comparison for greater than.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if \a x greater than \a y
	/// \retval false else
	bool isgreater(half x, half y);

	/// Comparison for greater equal.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if \a x greater equal \a y
	/// \retval false else
	bool isgreaterequal(half x, half y);

	/// Comparison for less than.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if \a x less than \a y
	/// \retval false else
	bool isless(half x, half y);

	/// Comparison for less equal.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if \a x less equal \a y
	/// \retval false else
	bool islessequal(half x, half y);

	/// Comarison for less or greater.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if either less or greater
	/// \retval false else
	bool islessgreater(half x, half y);

	/// Check if unordered.
	/// \param x first operand
	/// \param y second operand
	/// \retval true if unordered (one or two NaN operands)
	/// \retval false else
	bool isunordered(half x, half y);

	/// \}
	/// \name Casting
	/// \{

	/// Cast to or from half-precision floating point number.
	/// This casts between [half](\ref half_float::half) and any built-in arithmetic type. The values are converted directly 
	/// using the given rounding mode, without any roundtrip over `float` that a `static_cast` would otherwise do. It uses the 
	/// default rounding mode.
	///
	/// Using this cast with neither of the two types being a [half](\ref half_float::half) or with any of the two types 
	/// not being a built-in arithmetic type (apart from [half](\ref half_float::half), of course) results in a compiler 
	/// error and casting between [half](\ref half_float::half)s is just a no-op.
	/// \tparam T destination type (half or built-in arithmetic type)
	/// \tparam U source type (half or built-in arithmetic type)
	/// \param arg value to cast
	/// \return \a arg converted to destination type
	template<typename T,typename U> T half_cast(const U &arg);

	/// Cast to or from half-precision floating point number.
	/// This casts between [half](\ref half_float::half) and any built-in arithmetic type. The values are converted directly 
	/// using the given rounding mode, without any roundtrip over `float` that a `static_cast` would otherwise do.
	///
	/// Using this cast with neither of the two types being a [half](\ref half_float::half) or with any of the two types 
	/// not being a built-in arithmetic type (apart from [half](\ref half_float::half), of course) results in a compiler 
	/// error and casting between [half](\ref half_float::half)s is just a no-op.
	/// \tparam T destination type (half or built-in arithmetic type)
	/// \tparam R rounding mode to use (see [std::float_round_style](http://en.cppreference.com/w/cpp/types/numeric_limits/float_round_style))
	/// \tparam U source type (half or built-in arithmetic type)
	/// \param arg value to cast
	/// \return \a arg converted to destination type
	template<typename T,std::float_round_style R,typename U> T half_cast(const U &arg);
	/// \}


	/// Library-defined half-precision literals.
	/// Import this namespace to enable half-precision floating point literals (of course only if C++11 user-defined literals 
	/// are supported and enabled):
	/// ~~~~{.cpp}
	/// using namespace half_float::literal;
	/// half_float::half = 4.2_h;
	/// ~~~~
	namespace literal
	{
		/// Half literal.
		/// While this returns an actual half-precision value, half literals can unfortunately not be constant expressions due 
		/// to rather involved conversions.
		/// \param value literal value
		/// \return half with given value (if representable)
		half operator""_h(long double value);
	}
}


/// Extensions to the C++ standard library.
namespace std
{
	/// Numeric limits for half-precision floats.
	/// Because of the underlying single-precision implementation of many operations, it inherits some properties from 
	/// `std::numeric_limits<float>`.
	///
	/// **See also:** Documentation for [std::numeric_limits](http://en.cppreference.com/w/cpp/types/numeric_limits)
	template<> struct numeric_limits<half_float::half> : public std::numeric_limits<float>
	{
		/// Supports signed values.
		static constexpr bool is_signed = true;

		/// Is not exact.
		static constexpr bool is_exact = false;

		/// Doesn't provide modulo arithmetic.
		static constexpr bool is_modulo = false;

		/// IEEE conformant.
		static constexpr bool is_iec559 = true;

		/// Supports infinity.
		static constexpr bool has_infinity = true;

		/// Supports quiet NaNs.
		static constexpr bool has_quiet_NaN = true;

		/// Supports subnormal values.
		static constexpr std::float_denorm_style has_denorm = std::denorm_present;

		/// Rounding mode.
		/// Due to the mix of internal single-precision computations (using the rounding mode of the underlying 
		/// single-precision implementation) with the rounding mode of the single-to-half conversions, the actual rounding 
		/// mode might be `std::round_indeterminate` if the default half-precision rounding mode doesn't match the 
		/// single-precision rounding mode.
		static constexpr std::float_round_style round_style = /* unspecified */;

		/// Significant digits.
		static constexpr int digits = 11;

		/// Significant decimal digits.
		static constexpr int digits10 = 3;

		/// Required decimal digits to represent all possible values.
		static constexpr int max_digits10 = 5;

		/// Number base.
		static constexpr int radix = 2;

		/// One more than smallest exponent.
		static constexpr int min_exponent = -13;

		/// Smallest normalized representable power of 10.
		static constexpr int min_exponent10 = -4;

		/// One more than largest exponent
		static constexpr int max_exponent = 16;

		/// Largest finitely representable power of 10.
		static constexpr int max_exponent10 = 4;

		/// Smallest positive normal value.
		static constexpr half_float::half min() noexcept;

		/// Smallest finite value.
		static constexpr half_float::half lowest() noexcept;

		/// Largest finite value.
		static constexpr half_float::half max() noexcept;

		/// Difference between one and next representable value.
		static constexpr half_float::half epsilon() noexcept;

		/// Maximum rounding error.
		static constexpr half_float::half round_error() noexcept;

		/// Positive infinity.
		static constexpr half_float::half infinity() noexcept;

		/// Quiet NaN.
		static constexpr half_float::half quiet_NaN() noexcept;

		/// Signalling NaN.
		static constexpr half_float::half signaling_NaN() noexcept;

		/// Smallest positive subnormal value.
		static constexpr half_float::half denorm_min() noexcept;
	};

	/// Hash function for half-precision floats.
	/// This is only defined if C++11 `std::hash` is supported and enabled.
	///
	/// **See also:** Documentation for [std::hash](http://en.cppreference.com/w/cpp/utility/hash)
	template<> struct hash<half_float::half>
	{
		/// Type of function argument.
		typedef half_float::half argument_type;

		/// Function return type.
		typedef std::size_t result_type;

		/// Compute hash function.
		/// \param arg half to hash
		/// \return hash value
		std::size_t operator()(half_float::half arg) const;
	};
}
