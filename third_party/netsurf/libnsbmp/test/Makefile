DIR_TEST_ITEMS :=  decode_bmp:decode_bmp.c decode_ico:decode_ico.c

# Program fuzzer will use
DIR_FUZZER_ITEM := bmpdecode:decode_bmp.c
# Arguments to program
DIR_FUZZER_ARGS := "@@ /dev/null"
# Directory with inputs
DIR_FUZZER_INPUT := ns-afl-bmp
# Additional options to the fuzzer program
DIR_FUZZER_OPTS := -D

include $(NSBUILD)/Makefile.subdir
