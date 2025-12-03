"""
 Copyright (c) 2025 Mateusz Stadnik

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <https://www.gnu.org/licenses/>.
 """

import subprocess
import os
import glob

def search_tests():
    return glob.glob("c-testsuite/tests/single-exec/*.c")

excluded_tests = [
    "00174.c"
]

sources = search_tests()
sources = [s for s in sources if os.path.basename(s) not in excluded_tests]

def build_test(source):
    output = source.replace(".c", "")
    cmd = [
        "../../bin/armv8m-tcc",
        "-o", output,
        source,
    ]
    subprocess.run(cmd, check=True)
    return output

if __name__ == "__main__":
    for source in sources:
        print(f"Building test: {source}")
        build_test(source)