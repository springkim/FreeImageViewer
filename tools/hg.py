import hashlib,json,os

f="hg.json";s="CORE/IMAGE_OPEN.H"
h="HG_"+hashlib.md5(s.encode()).hexdigest().upper()+"_H";print(h)
try: import pyperclip;pyperclip.copy(h)
except ImportError: pass
d=json.load(open(f)) if os.path.exists(f) else {};d[h]=s
json.dump(d,open(f,"w"),ensure_ascii=False,indent=4)