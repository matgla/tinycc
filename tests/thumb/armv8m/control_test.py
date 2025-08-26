from . import utils 

def test_clrex():
    utils.perform_test_for_file("test_clrex.S")

def test_it():
    utils.perform_test_for_file("test_it.S")

def test_bkpt():
    utils.perform_test_for_file("test_bkpt.S")