import re,os,sys,subprocess
s=open('mca_web.c',encoding='utf-8').read()
out=[];i=0;n=len(s);st='code'
while i<n:
    if st=='code':
        if s[i]=='"': st='str'; out.append(s[i]); i+=1
        elif s.startswith('/*',i):
            j=s.find('*/',i+2); i=(j+2) if j>0 else n
        elif s.startswith('//',i):
            j=s.find('\n',i); i=j if j>0 else n
        else: out.append(s[i]); i+=1
    else:
        if s[i]=='\\': out.append(s[i:i+2]); i+=2
        elif s[i]=='"': st='code'; out.append(s[i]); i+=1
        else: out.append(s[i]); i+=1
clean=''.join(out)
os.makedirs('/tmp/js3',exist_ok=True)
bad=0
for m in re.finditer(r'static const char (\w+)\[\]\s*=(.*?);\n', clean, re.S):
    name=m.group(1)
    html=''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))).replace('\\"','"')
    for sm in re.finditer(r'<script>(.*?)</script>', html, re.S):
        js=sm.group(1); fn=f'/tmp/js3/{name}.js'
        open(fn,'w',encoding='utf-8').write(js)
        r=subprocess.run(['node','--check',fn],capture_output=True,text=True)
        ok = r.returncode==0
        # ссылки на функции
        defined=set(re.findall(r'function\s+(\w+)',js))
        used=set(re.findall(r'onclick=(\w+)',js)) | set(re.findall(r'onchange=(\w+)',js))
        miss=[u for u in used if u not in defined]
        # inline onclick в строках HTML
        inl=set(re.findall(r'onclick=\\?"(\w+)\(',js))
        miss += [u for u in inl if u not in defined]
        print(f'{name:12s} синтаксис: {"ОК" if ok else "ОШИБКА"}', end='')
        if not ok: print('\n   ', r.stderr.strip().split('\n')[-3:]); bad+=1
        elif miss: print(f'  НЕТ ФУНКЦИЙ: {miss}'); bad+=1
        else: print('  ссылки: ОК')
sys.exit(1 if bad else 0)
