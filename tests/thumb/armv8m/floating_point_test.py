from . import utils

def test_vpop():
    utils.perform_test_for_file("test_vpop.S")

def test_vpush():
    utils.perform_test_for_file("test_vpush.S")

def test_vfp():
    utils.perform_test_for_file("test_vfp.S")

def test_vcvt_rounding():
    utils.perform_test_for_file("test_vcvt_rounding.S")
