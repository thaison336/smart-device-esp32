#!/usr/bin/env python3
import os
import sys
import subprocess

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    proto_dir = os.path.join(project_root, "main", "stm32_bridge", "proto")
    proto_file = os.path.join(proto_dir, "datalogger.proto")

    print(f"Generating Nanopb files for {proto_file}...")
    
    python_exe = sys.executable
    cmd = [
        python_exe, "-m", "nanopb.generator.nanopb_generator",
        "-I", proto_dir,
        f"--output-dir={proto_dir}",
        proto_file
    ]
    
    try:
        subprocess.run(cmd, check=True)
        print("Successfully generated Nanopb files:")
        print(os.path.join(proto_dir, "datalogger.pb.h"))
        print(os.path.join(proto_dir, "datalogger.pb.c"))
    except Exception as e:
        print(f"Error running nanopb generator: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
