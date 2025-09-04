from . import utils 

def test_bx():
    utils.perform_test_for_file("test_bx.S")
    
def test_bl():
    utils.perform_test_for_file("test_bl.S")

def test_blx():
    utils.perform_test_for_file("test_blx.S")

def is_compatible_branch_instruction(a, b):
    branch_set = ["b", "b.n", "b.w"]
    if a in branch_set and b in branch_set:
        return True
    return False 
 
def test_b():
    output_file = utils.compile_code("test_b.S")
    disassembly_sut = utils.disassemble_code(output_file).splitlines()
    expected_file = output_file.parent.parent / "expected" / output_file.name
    disassembly_expected = utils.disassemble_code(expected_file).splitlines()
    sut = utils.cleanup_dissambly(disassembly_sut)
    expected = utils.cleanup_dissambly(disassembly_expected) 
    assert len(expected) > 0, "Expected disassembly is empty"
    
    for i in range(len(expected)):
        if (sut[i][1] != expected[i][1]):
            # try to determine if compatible instruction is being used
            if is_compatible_branch_instruction(sut[i][2], expected[i][2]):
                sut_label = sut[i][3].split("<")[-1][:-1]
                expected_label = expected[i][3].split("<")[-1][:-1]
                assert sut_label == expected_label, f"Mismatch at line {i}: {sut[i]} != {expected[i]}"   
                continue
        
        assert sut[i][1] == expected[i][1], f"Mismatch at line {i}: {sut[i]} != {expected[i]}"   
