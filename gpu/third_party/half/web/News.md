News														{#news}
====

March 6, 2017 - Release 1.12.0
------------------------------

[Version 1.12.0](http://sourceforge.net/projects/half/files/half/1.12.0) of the library has been released. It changes the behaviour of half_cast() when casting to/from builtin floating point types. Previously those were performed by an explicit intermediary cast to/from `float`. But this could lead to imprecise results, since the rounding from `double`/`long double` to `float` was not controlled by the rounding mode of the half_cast(). Now each half_cast() always rounds directly to/from the specified type using the given rounding mode and thus guarantees exact compliance to this rounding mode. Furthermore, a minor portability issue with logb() and ilogb() has been fixed, that could cause trouble on non-twos-complement implementations (if there are any at all). It was also tested for *MS Visual C++ 2015*.

----------------------------------
November 16, 2013 - Release 1.11.0
----------------------------------

[Version 1.11.0](http://sourceforge.net/projects/half/files/half/1.11.0) of the library has been released. It further increases the flexibility of the rounding, by allowing the round to nearest behaviour to be configured more precisely. By default half-way cases during round to nearest are rounded away from zero (and thus analogous to the round() function), but by redefining [HALF_ROUND_TIES_TO_EVEN](\ref HALF_ROUND_TIES_TO_EVEN) to `1` this can be changed to the slower but less biased round to even. In addition to that the support for C++11 mathematical functions in light of unsupported single-precision versions has been completed by providing erf(), erfc(), lgamma() and tgamma() even if their `<cmath>` counterparts are unavailable. Furthermore, it fixes a bug that made it impossible to disable support for C++11 mathematical functions in *VC++ 2013*.

---------------------------------
November 9, 2013 - Release 1.10.0
---------------------------------

[Version 1.10.0](http://sourceforge.net/projects/half/files/half/1.10.0) of the library has been released. It adds the possibility of changing the default rounding mode for halfs (used for any float-to-half conversion) at compile-time by redefining the [HALF_ROUND_STYLE](\ref HALF_ROUND_STYLE) preprocessor symbol. In addition to that the behaviour of the half_cast() function was adjusted to respect the specified rounding mode for conversions to/from integer types, without a roundtrip over an additional conversion to/from `float`. The half_cast() now only supports built-in arithmetic types (apart from [half](\ref half_float::half)s, of course) and any further conversions from them to custom types have to be done explicitly. The usage and checks of C++11 features has been extended by support for type traits and TMP facilities from `<type_traits>`, controllable with the `HALF_ENABLE_CPP11_TYPE_TRAITS` symbol. Last but not least, the library now supports non-IEEE single-precision implementations, which are pretty unlikely nowadays, though.

--------------------------------
November 1, 2013 - Release 1.9.2
--------------------------------

[Version 1.9.2](http://sourceforge.net/projects/half/files/half/1.9.2) of the library has been released. It was tested and fixed for *MS Visual C++ 2013* and *gcc 4.8*. In addition to that it removes some warnings on *MSVC* compilers in general.

------------------------------
August 8, 2013 - Release 1.9.1
------------------------------

[Version 1.9.1](http://sourceforge.net/projects/half/files/half/1.9.1) of the library has been released. It fixes compilation problems with older *gcc* and *MSVC* versions. Furthermore it adds a small fix to the non-C++11 implementations of remainder() and remquo(), which could cause a missing symbol on implementations that do not bring `<cmath>` functions into global namespace.

------------------------------
August 7, 2013 - Release 1.9.0
------------------------------

[Version 1.9.0](http://sourceforge.net/projects/half/files/half/1.9.0) of the library has been released. This version fixes the behaviour of the half-precsion C++11 functions `nearbyint`, `rint`, `lrint` and `llrint`. Formerly those functions were implemented in terms of their respective single-precision versions from `<cmath>` (and thus were only supported if the single-precision versions were available, too) and were thus using the current rounding mode of the underlying single-precision implementation. But it is conceptually more correct for those functions to use the rounding mode of the half-precision implementation (truncation). So now those functions are always supported and return the nearest integer according to [half](\ref half_float::half)'s internal rounding mode and thus behave equivalently to trunc() and `static_cast<long>` respectively. Furthermore the support for C++11 mathematical functions in light of unsupported single-precision versions has been increased further, by providing remainder(), remquo() and cbrt() even if their `<cmath>` counterparts are unavailable.

--------------------------------
January 22, 2013 - Release 1.8.1
--------------------------------

[Version 1.8.1](http://sourceforge.net/projects/half/files/half/1.8.1) of the library has been released. This just fixes a compile error when including `half.hpp` in multiple files, resulting in multiple definitions of the nanh() function due to a missing `inline` specification.

--------------------------------
January 19, 2013 - Release 1.8.0
--------------------------------

[Version 1.8.0](http://sourceforge.net/projects/half/files/half/1.8.0) of the library has been released. It adds support for a bunch of additional C++11 mathematical functions even if their single-precision counterparts are not supported, in particular exponential and logarithmic functions (exp2(), expm1(), log2(), log1p()), hyperbolic area functions (asinh(), acosh(), atanh()) and the hypotenuse function (hypot()). The fma() function now uses the default single-precision implementation even if the single-precision version from `<cmath>` is available but not faster than the straight-forward implementation. Thus it is now always at least equally fast to the manual half-precision `x*y + z` operation (yet being correctly rounded as a single operation) and thus [FP_FAST_FMAH](\ref FP_FAST_FMAH) practically always defined.

Furthermore, the internal expression implementation has been completely revised. This fixes issues with overload resolution which could occur when trying to call certain mathematical functions by unqualified invocation (relying on `using` declarations or ADL) and led to ambiguities or the incorrect preference of the standard library functions over the half-precision versions.

--------------------------------
October 26, 2012 - Release 1.7.0
--------------------------------

[Version 1.7.0](http://sourceforge.net/projects/half/files/half/1.7.0) of the library has been released. It adds support for C++11 `noexcept` specifications. But due to the ongoing discussions about their appropriate usage in practice (and the author's limited experience with them) they are at the moment only used to provide a C++11-conformant `std::numeric_limits` specialization, with all its functions returning half-preicision constants now properly being `noexcept`. Furthermore, the automatic support for C++11 `long long` and the corresponding mathematical functions on *Microsoft* compilers has been extended all the way down to *VC++ 2003*.

----------------------------------
September 13, 2012 - Release 1.6.1
----------------------------------

[Version 1.6.1](http://sourceforge.net/projects/half/files/half/1.6.1) of the library has been released. It makes the fma() and fdim() functions available even if the corresponding single-precision functions from C++11 `<cmath>` are not supported.

----------------------------------
September 12, 2012 - Release 1.6.0
----------------------------------

[Version 1.6.0](http://sourceforge.net/projects/half/files/half/1.6.0) of the library has been released. It fixes a major bug in the C++98 compatibility for non-VC compilers, which required C++11 `<cmath>` functions even without C++11 support enabled. Furthermore, an additional preprocessor flag `HALF_ENABLE_CPP11_LONG_LONG` has been added, which controls the support for C++11 `long long` integers and the corresponding mathematical functions (actually only llround()).

-------------------------------
August 17, 2012 - Release 1.5.1
-------------------------------

[Version 1.5.1](http://sourceforge.net/projects/half/files/half/1.5.1) of the library has been released. It fixes the `std::numeric_limits` specialization to always return `std::round_indeterminate` as `round_style`, even if the `float` version returns `std::round_toward_zero`. This is neccessary as the single-to-half conversion is not exactly round-toward-zero, since it maps overflows to +/-infinity instead of the maximum/minimum finite value.

-------------------------------
August 16, 2012 - Release 1.5.0
-------------------------------

[Version 1.5.0](http://sourceforge.net/projects/half/files/half/1.5.0) of the library has been released. It adds a special half_cast() that can be used to cast between [half](\ref half_float::half) and any type convertible to/from `float`. In addition to doing an explicit conversion via `float` and thus avoiding any possible warnings about precision loss, it allows the explicit specification of the rounding mode to use for the single-to-half conversion (if any). This way you can choose a specific rounding mode if needed and can even synchronize it with the rounding mode of the underlying single-precision implementation (which is usually `std::round_to_nearest`). But this is more of an expert feature to be used when you are sure you really need it (which should rarely be the case) and most of the time the default rounding from single- to half-precision (`std::round_indeterminate`), which is the fastest, will be sufficient. Furthermore, the library has been tested successfully with Visual C++ 2008.

-------------------------------
August 12, 2012 - Release 1.4.0
-------------------------------

[Version 1.4.0](http://sourceforge.net/projects/half/files/half/1.4.0) of the library has been released. It adds support for C++11 generalized constant expressions (`constexpr`). But due to the not constantly expressable half-single-conversions most arithmetic operators as well as the half-precision literals cannot be made `constexpr`. The only `constexpr` operators are the unary plus and minus as well as the zero-initializing default constructor. But now the `std::numeric_limits` specialization is C++11-conformant, with all its special half-precision constants being actual constant expressions.

-------------------------------
August 11, 2012 - Release 1.3.1
-------------------------------

[Version 1.3.1](http://sourceforge.net/projects/half/files/half/1.3.1) of the library has been released. It fixes a bug with non-VC compilers that required the presence of `std::isnan` and `std::signbit` even if C++11 `<cmath>` functions are unsupported/disabled. Furthermore the library has been tested successfully with Visual C++ 2005 and 2012.

-------------------------------
August 10, 2012 - Release 1.3.0
-------------------------------

[Version 1.3.0](http://sourceforge.net/projects/half/files/half/1.3.0) of the library has been released. It makes the requirement for C++11's `<cstdint>` and `static_assert` optional and thus is now comletely C++98-compatible. Furthermore, all the C++11 features used by the library and detected automatically are now overridable by the user, by just defining the corresponding preprocessor symbols to 0 or 1, which is useful in cases where the automatic detection fails or where the support for a feature should be explicitly disabled.

------------------------------
August 7, 2012 - Release 1.2.0
------------------------------

[Version 1.2.0](http://sourceforge.net/projects/half/files/half/1.2.0) of the library has been released. It adds preprocessor definitions for [HUGE_VALH](\ref HUGE_VALH) and (maybe) [FP_FAST_FMAH](\ref FP_FAST_FMAH) in correspondence with their respective single-precision counterparts from `<cmath>`. It also fixes the preprocessor symbols it uses internally to be undefined properly after their use. Furthermore, the library has been tested successfully with *clang 3.1*.

------------------------------
August 7, 2012 - Release 1.1.2
------------------------------

[Version 1.1.2](http://sourceforge.net/projects/half/files/half/1.1.2) of the library has been released. It fixes the `std::numeric_limits` specialization further, enabling it to return `std::round_toward_zero` instead of `std::round_indeterminate` when possible (though this is very unlikely). Additionally the `round_error()` has been corrected to reflect the worst-case round-toward-zero behaviour. Furthermore, the library has been tested successfully with *gcc 4.5*.

------------------------------
August 6, 2012 - Release 1.1.1
------------------------------

[Version 1.1.1](http://sourceforge.net/projects/half/files/half/1.1.1) of the library has been released. It includes a minor fix to the `std::numeric_limits` specialization, which now returns a normalized number for `min()` according to the C++ standard. Additionally the `round_style` is now `std::round_indeterminate` due to the mixture of internal single-precision rounding with float-to-half truncation.

------------------------------
August 6, 2012 - Release 1.1.0
------------------------------

[Version 1.1.0](http://sourceforge.net/projects/half/files/half/1.1.0) of the library has been released. It provides support for explicit half-precision literals using C++11's user-defined literals. It has been tested successfully with *gcc 4.7*.

------------------------------------
August 5, 2012 - First release 1.0.0
------------------------------------

The first official release, [version 1.0.0](http://sourceforge.net/projects/half/files/half/1.0.0), of the library is finally out.

---------------------------------------------
August 1, 2012 - Code and Project-Web release
---------------------------------------------

The first version of the library has been checked into source control and is thus available to the public through the [SVN repository](http://sourceforge.net/p/half/code/), though still waiting for an explicit file release. Additionally the project web containing the library's documentation went online.

--------------------------------
July 30, 2012 - Project Kick-off
--------------------------------

The **half** project has finally been registered on [Sourceforge.net](http://sourceforge.net/projects/half/).
