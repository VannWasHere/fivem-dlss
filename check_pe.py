import struct
import sys

def read_pe_resources(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    # Find .rsrc section
    idx = data.find(b'.rsrc')
    if idx == -1:
        print("No .rsrc section found")
        return
    
    print(f"Found .rsrc at offset {idx}")
    
    # Look for FX_ASI_BUILD in the entire file
    fx_idx = data.find(b'FX_ASI_BUILD')
    if fx_idx != -1:
        print(f"Found FX_ASI_BUILD at offset {fx_idx}")
        # Print surrounding bytes
        context = data[fx_idx-20:fx_idx+50]
        print(f"Context: {context}")
    else:
        print("FX_ASI_BUILD not found in file")
    
    # Also search for the type name encoded differently
    fx_unicode = 'FX_ASI_BUILD'.encode('utf-16-le')
    fx_idx2 = data.find(fx_unicode)
    if fx_idx2 != -1:
        print(f"Found FX_ASI_BUILD (Unicode) at offset {fx_idx2}")

if __name__ == '__main__':
    read_pe_resources(sys.argv[1])
