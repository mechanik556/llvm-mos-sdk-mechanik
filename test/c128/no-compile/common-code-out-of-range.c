// The Common-RAM code area must lie within $0000-$0FFF (4K Common RAM). This is
// linked with -Wl,--defsym=__c128_common_code_origin=0x0F80 (see this
// directory's CMakeLists.txt), which puts the default 512-byte area past $0FFF:
// the link must fail.
int main(void) { return 0; }
