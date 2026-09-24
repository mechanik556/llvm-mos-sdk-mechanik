set_directory_properties(PROPERTIES EXCLUDE_FROM_ALL ON)

# WILL_FAIL alone would pass on any failure, including an unrelated compile
# error, so also require the expected linker diagnostic.
add_no_compile_test(ram-too-big)
add_no_compile_test(bank1-data-too-big)
add_no_compile_test(common-code-out-of-range)
target_link_options(common-code-out-of-range PRIVATE
  -Wl,--defsym=__c128_common_code_origin=0x0F80)

set_tests_properties(ram-too-big-no-compile PROPERTIES
  WILL_FAIL NO PASS_REGULAR_EXPRESSION "will not fit in region 'ram'")
set_tests_properties(bank1-data-too-big-no-compile PROPERTIES
  WILL_FAIL NO PASS_REGULAR_EXPRESSION "will not fit in region 'bank1'")
set_tests_properties(common-code-out-of-range-no-compile PROPERTIES
  WILL_FAIL NO PASS_REGULAR_EXPRESSION "Common-RAM code area must lie within")
