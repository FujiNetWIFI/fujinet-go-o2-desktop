# The machine's tests and tools. o2_headless is the side of the MAME A/B runs
# that is ours (tools/ab/): built, not registered.
o2_test(o2_headless o2_headless.c)

# CONFIG on the stand-in with a generated font, against frame hashes made by
# MAME (tools/ab/config_golden.py): no ROM needed.
o2_test(config_frames_test config_frames_test.c)
add_test(NAME config_frames COMMAND config_frames_test)

# The stand-in's readkey and getjoystick, with a cartridge of the tests' own.
o2_test(stub_test stub_test.c)
add_test(NAME stub_boot COMMAND stub_test)

# The FujiNet cartridge and its transport against a stand-in FujiNet on
# loopback: a MOUNT_IMAGE with a pushed image, the staging, the swap at the next
# fetch of $400 and the trace tap held to the bytes on the wire. POSIX (77).
o2_test(cart_test cart_test.c)
add_test(NAME cart COMMAND cart_test)
set_tests_properties(cart PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
