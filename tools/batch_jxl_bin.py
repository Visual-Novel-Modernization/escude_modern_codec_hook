import sys, os, time, struct, tempfile
from concurrent.futures import ThreadPoolExecutor
import subprocess

K = 0x65AC9365
M = 0xffffffff
CJXL_PATH = os.environ.get("CJXL_PATH", "cjxl")

def f(x):
    t = x ^ K
    return (t ^ ((8 * (t ^ ((2 * t) & M))) & M) ^ ((t ^ (t >> 1)) >> 3)) & M

def unpack_archive(path):
    with open(path, 'rb') as fh:
        h = fh.read(20)
        assert h[:8] == b'ESC-ARC2'
        a, b, c = struct.unpack('<3I', h[8:])
        v20 = f(a); count = v20 ^ b; v21 = f(v20); nsz = v21 ^ c
        raw = bytearray(fh.read(12 * count))
        names = fh.read(nsz)
        
        v = v21
        out = []
        for i in range(3 * count):
            v = f(v)
            out.append(struct.unpack_from('<I', raw, 4 * i)[0] ^ v)
            
        files = []
        for i in range(count):
            no, off, sz = out[3 * i : 3 * i + 3]
            e = names.index(b'\0', no)
            name = names[no:e].decode('cp932')
            fh.seek(off)
            data = fh.read(sz)
            files.append((name, data))
    return files

def pack_archive(files, out_path, seed=0x12345678):
    def mbs_key(item):
        name = item[0]
        try:
            return name.lower().encode('cp932')
        except:
            return name.lower().encode('utf-8', 'ignore')
            
    files = sorted(files, key=mbs_key)
    count = len(files)
    
    names_buf = bytearray()
    name_offsets = []
    for name, _ in files:
        no = len(names_buf)
        name_offsets.append(no)
        names_buf.extend(name.encode('cp932') + b'\0')
        
    nsz = len(names_buf)
    header_len = 20 + 12 * count + nsz
    cur_off = header_len
    
    table_plain = []
    file_datas = []
    for (name, data), no in zip(files, name_offsets):
        sz = len(data)
        table_plain.extend([no, cur_off, sz])
        cur_off += sz
        file_datas.append(data)
        
    a = seed & M
    v20 = f(a)
    b = v20 ^ count
    v21 = f(v20)
    c = v21 ^ nsz
    
    v = v21
    table_enc = bytearray()
    for x in table_plain:
        v = f(v)
        table_enc.extend(struct.pack('<I', x ^ v))
        
    with open(out_path, 'wb') as fh:
        fh.write(b'ESC-ARC2')
        fh.write(struct.pack('<3I', a, b, c))
        fh.write(table_enc)
        fh.write(names_buf)
        for data in file_datas:
            fh.write(data)

def encode_one_png(args):
    idx, name, data, tmp_dir = args
    if not name.lower().endswith(".png"):
        return idx, name, data
    
    in_file = os.path.join(tmp_dir, f"in_{idx}.png")
    out_file = os.path.join(tmp_dir, f"out_{idx}.jxl")
    try:
        with open(in_file, "wb") as f:
            f.write(data)
        cmd = [CJXL_PATH, in_file, out_file, "-e", "7", "-q", "90"]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode == 0 and os.path.exists(out_file):
            with open(out_file, "rb") as f:
                jxl_data = f.read()
            return idx, name, jxl_data
        else:
            print(f"[!] Warning: failed to encode {name}, keeping original")
            return idx, name, data
    finally:
        if os.path.exists(in_file): os.remove(in_file)
        if os.path.exists(out_file): os.remove(out_file)

def process_bin_file(bin_path, workers=8):
    bin_dir = os.path.dirname(bin_path)
    bin_name = os.path.basename(bin_path)
    bak_path = bin_path + ".bak"
    out_tmp = bin_path + ".new"
    
    print(f"\n=======================================================")
    print(f"[*] Processing {bin_name}...")
    print(f"=======================================================")
    
    t0 = time.time()
    files = unpack_archive(bin_path)
    print(f"[+] Unpacked {len(files)} files in {time.time()-t0:.2f}s")
    
    png_items = [(i, name, data) for i, (name, data) in enumerate(files) if name.lower().endswith(".png")]
    print(f"[+] Total PNGs to transcode: {len(png_items)} (using {workers} workers)")
    
    tmp_dir = os.path.join(bin_dir, f"_tmp_{os.path.splitext(bin_name)[0]}")
    os.makedirs(tmp_dir, exist_ok=True)
    
    tasks = [(i, name, data, tmp_dir) for i, name, data in png_items]
    
    reencoded_map = {}
    completed = 0
    t_start = time.time()
    with ThreadPoolExecutor(max_workers=workers) as executor:
        for idx, name, new_data in executor.map(encode_one_png, tasks):
            reencoded_map[idx] = new_data
            completed += 1
            if completed % 50 == 0 or completed == len(png_items):
                elapsed = time.time() - t_start
                fps = completed / elapsed if elapsed > 0 else 0
                print(f"    [{completed}/{len(png_items)}] ({fps:.1f} imgs/sec, elapsed {elapsed:.1f}s)")
                
    try:
        os.rmdir(tmp_dir)
    except:
        pass
        
    # Reassemble file list
    new_files = []
    orig_total_bytes = 0
    new_total_bytes = 0
    for i, (name, data) in enumerate(files):
        orig_total_bytes += len(data)
        if i in reencoded_map:
            d = reencoded_map[i]
            new_files.append((name, d))
            new_total_bytes += len(d)
        else:
            new_files.append((name, data))
            new_total_bytes += len(data)
            
    print(f"[*] Repacking {bin_name}...")
    pack_archive(new_files, out_tmp)
    
    orig_mb = os.path.getsize(bin_path) / (1024*1024)
    new_mb = os.path.getsize(out_tmp) / (1024*1024)
    ratio = (1.0 - (new_mb / orig_mb)) * 100
    print(f"[+] Repack complete: {orig_mb:.1f} MB -> {new_mb:.1f} MB ({ratio:.1f}% reduction)")
    
    # Backup original and replace
    if not os.path.exists(bak_path):
        os.rename(bin_path, bak_path)
        print(f"[+] Renamed original to {os.path.basename(bak_path)}")
    else:
        print(f"[*] {os.path.basename(bak_path)} already exists, removing original")
        os.remove(bin_path)
        
    os.rename(out_tmp, bin_path)
    print(f"[+] Replaced {bin_name} successfully!")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        process_bin_file(p, workers=8)
