def test_boot_message(dut):
    dut.expect("Hello world", timeout=1)
