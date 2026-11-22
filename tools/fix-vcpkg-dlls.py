import os, re, shutil, glob, sys
root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "vcpkg_installed", "x64-windows-clang")
for tf in glob.glob(root + "/share/*/*[Tt]argets*.cmake"):
    txt = open(tf, errors="ignore").read()
    for rel in set(re.findall(r"\$\{_IMPORT_PREFIX\}/(tools/[^\"\) ]+\.dll)", txt)):
        dst = os.path.join(root, rel)
        src = os.path.join(root, "bin", os.path.basename(rel))
        if not os.path.exists(dst) and os.path.exists(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            print("dll-fix:", rel)
