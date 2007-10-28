MACRO(DETECT_COMPILER)
IF(NOT CMAKE_COMPILER_IS_GNUCC)
  CHECK_C_SOURCE_COMPILES(
"#ifndef __INTEL_COMPILER
#error no icc
#endif
int main(){}
" CMAKE_COMPILER_IS_ICC)
ENDIF(NOT CMAKE_COMPILER_IS_GNUCC)
ENDMACRO(DETECT_COMPILER)

MACRO(MACRO_TEST ARG VAR)
CHECK_C_SOURCE_COMPILES(
"#ifndef ${ARG}
#error ${ARG} macro not defined
#endif
int main(){}
" ${VAR})
ENDMACRO(MACRO_TEST)