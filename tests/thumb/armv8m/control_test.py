from . import utils

def test_clrex():
    utils.perform_test_for_file("test_clrex.S")

def test_cps():
    utils.perform_test_for_file("test_cps.S")

def test_csdb():
    utils.perform_test_for_file("test_csdb.S")

def test_dmb():
    utils.perform_test_for_file("test_dmb.S")

def test_dsb():
    utils.perform_test_for_file("test_dsb.S")

def test_it():
    utils.perform_test_for_file("test_it.S")

def test_isb():
    utils.perform_test_for_file("test_isb.S")

def test_bkpt():
    utils.perform_test_for_file("test_bkpt.S")

def test_nop():
    utils.perform_test_for_file("test_nop.S")

def test_sev():
    utils.perform_test_for_file("test_sev.S")

def test_ssbb():
    utils.perform_test_for_file("test_ssbb.S")

def test_tt():
    utils.perform_test_for_file("test_tt.S")

def test_udf():
    utils.perform_test_for_file("test_udf.S")

def test_wfe():
    utils.perform_test_for_file("test_wfe.S")

def test_wfi():
    utils.perform_test_for_file("test_wfi.S")

def test_yield():
    utils.perform_test_for_file("test_yield.S")
