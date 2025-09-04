from . import utils 

def test_adc_imm():
    utils.perform_test_for_file("test_adc_imm.S")
    
def test_adc_imm():
    utils.perform_test_for_file("test_adc_reg.S")
    
def test_mov_imm():
    utils.perform_test_for_file("test_mov_imm.S")

def test_cmp_imm():
    utils.perform_test_for_file("test_cmp_imm.S") 
    
