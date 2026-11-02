# The machine's tests and tools. o2_headless is the side of the MAME A/B runs
# that is ours (tools/ab/): built, not registered.
o2_test(o2_headless o2_headless.c)

# CONFIG on the stand-in with a generated font, against frame hashes made by
# MAME (tools/ab/config_golden.py): no ROM needed.
o2_test(config_frames_test config_frames_test.c)
add_test(NAME config_frames COMMAND config_frames_test)
