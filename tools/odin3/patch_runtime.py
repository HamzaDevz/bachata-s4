"""Swap host/shadps4-arm64-fex in runtime.zip and re-pin it in manifest.json."""
import hashlib
import json
import os
import sys
import zipfile

runtime_dir, core_path, runtime_version = sys.argv[1:4]
TARGET = "host/shadps4-arm64-fex"
zip_path = os.path.join(runtime_dir, "runtime.zip")
manifest_path = os.path.join(runtime_dir, "manifest.json")

with open(core_path, "rb") as f:
    core = f.read()

tmp_path = zip_path + ".tmp"
with zipfile.ZipFile(zip_path) as src, zipfile.ZipFile(tmp_path, "w") as dst:
    for info in src.infolist():
        data = core if info.filename == TARGET else src.read(info.filename)
        dst.writestr(info, data, compress_type=info.compress_type)
os.replace(tmp_path, zip_path)

with open(manifest_path) as f:
    manifest = json.load(f)
manifest["runtimeVersion"] = runtime_version
entry = next(e for e in manifest["files"] if e["path"] == TARGET)
entry["size"] = len(core)
entry["sha256"] = hashlib.sha256(core).hexdigest()
with open(manifest_path, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
print(runtime_version, entry["size"], entry["sha256"])
