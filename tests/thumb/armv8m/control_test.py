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