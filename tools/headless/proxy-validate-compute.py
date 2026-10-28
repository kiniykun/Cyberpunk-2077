"""Proxy GPU validation for converted static compute WGSL that the headless Deno device cannot run as-is.

Deno's wgpu device has no `subgroups` (and naga rejects `enable subgroups;`) and no texture-formats-tier1/tier2.
For every complete row of build/shaders/static-compute-conversion.json whose real Deno result
(static-compute-conversion-gpu-validation.json) is not OK, a scratch copy is made with only these substitutions:
  - `enable subgroups;` removed; subgroupBroadcastFirst(x) -> (x); quadBroadcast(x, i) -> (x); entry builtins
    subgroup_invocation_id / subgroup_size -> 0u / 32u                                         [runtime cap: subgroups]
  - tier1 storage formats -> a core format with the same texel type (rg11b10ufloat -> rgba16float, ...)  [cap: tier1]
and validated (module + compute pipeline, layout auto) by validate-compute.ts on a device with the adapter's compute
limits. The shipped WGSL is never changed. Results go to static-compute-conversion-proxy-validation.json keyed by
runtime name with the exact WGSLSHA256 of the shipped file, which the package generators check together with the
runtime requirement bits (a substitution is accepted only if a requiredCaps bit covers it).

Usage: proxy-validate-compute.py [names...]   (default: every complete row whose real validation is not OK)
"""
import hashlib, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'build/shaders'
MANIFEST = SHADERS / 'static-compute-conversion.json'
GPU = SHADERS / 'static-compute-conversion-gpu-validation.json'
PROXY = SHADERS / 'static-compute-conversion-proxy-validation.json'
WORK = ROOT / 'build/static-compute/proxy'
VALIDATOR = ROOT / 'tools/headless/validate-compute.ts'
DENO_CANDIDATES = [Path.home() / 'AppData/Local/Microsoft/WinGet/Packages/DenoLand.Deno_Microsoft.Winget.Source_8wekyb3d8bbwe/deno.exe',
                   Path.home() / '.deno/bin/deno.exe']
TIER1_PROXY_FORMAT = {'rg11b10ufloat': 'rgba16float', 'r16float': 'r32float', 'rg16float': 'rgba16float',
                      'r8unorm': 'rgba8unorm', 'rg8unorm': 'rgba8unorm', 'rgb10a2unorm': 'rgba8unorm',
                      'r16unorm': 'r32float', 'rg16unorm': 'rgba16float', 'rgba16unorm': 'rgba16float',
                      'r8uint': 'r32uint', 'rg8uint': 'rgba8uint', 'r16uint': 'r32uint', 'rg16uint': 'rgba16uint',
                      'r8sint': 'r32sint', 'rg8sint': 'rgba8sint', 'r16sint': 'r32sint', 'rg16sint': 'rgba16sint',
                      'rgb10a2uint': 'rgba16uint'}


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def replace_call(text, function, template='({})'):
    """Replace function(a[, b...]) by template.format(a), matching parentheses; returns (text, count)."""
    out = []; at = 0; count = 0
    pattern = re.compile(r'\b' + function + r'\s*\(')
    while True:
        m = pattern.search(text, at)
        if not m: out.append(text[at:]); break
        depth = 1; i = m.end(); first_end = None
        while depth:
            c = text[i]
            if c == '(': depth += 1
            elif c == ')': depth -= 1
            elif c == ',' and depth == 1 and first_end is None: first_end = i
            i += 1
        out.append(text[at:m.start()]); out.append(template.format(text[m.end():first_end if first_end else i - 1]))
        at = i; count += 1
    return ''.join(out), count


def proxy(text):
    subs = []
    if re.search(r'^\s*enable\s+subgroups\s*;', text, re.M):
        text = re.sub(r'^\s*enable\s+subgroups\s*;\s*\n', '', text, flags=re.M)
        for fn in ('subgroupBroadcastFirst', 'quadBroadcast', 'subgroupOr', 'subgroupAnd', 'subgroupMax', 'subgroupMin',
                   'subgroupAdd'):
            text, n = replace_call(text, fn)
            if n or fn in ('subgroupBroadcastFirst', 'quadBroadcast'): subs.append(f'{fn}->identity x{n}')
        # subgroupBallot(b) -> vec4<u32>(u32(b), 0u, 0u, 0u): same result type, enough for module/pipeline validation.
        text, n = replace_call(text, 'subgroupBallot', 'vec4<u32>(u32({}), 0u, 0u, 0u)')
        if n: subs.append(f'subgroupBallot->vec4 x{n}')
        for builtin, value in (('subgroup_invocation_id', '0u'), ('subgroup_size', '32u')):
            m = re.search(r',?\s*@builtin\(' + builtin + r'\)\s*(\w+)\s*:\s*u32', text)
            if m:
                text = text[:m.start()] + text[m.end():]
                text = re.sub(r'\b' + m.group(1) + r'\b', value, text)
                subs.append(f'{builtin}->{value}')
        if re.search(r'\b(subgroup\w+|quad\w+)\s*\(|@builtin\(subgroup', text):
            raise ValueError('subgroup/quad builtin without a proxy substitution')
    for fmt, core in TIER1_PROXY_FORMAT.items():
        text, n = re.subn(r'(texture_storage_\w+<\s*)' + fmt + r'(\s*,)', r'\g<1>' + core + r'\g<2>', text)
        if n: subs.append(f'{fmt}->{core}')
    subs.append('maxComputeInvocationsPerWorkgroup=adapter')
    return text, subs


def main():
    manifest = json.loads(MANIFEST.read_text())
    gpu = json.loads(GPU.read_text()) if GPU.exists() else {}
    names = set(sys.argv[1:])
    rows = [r for r in manifest['Shaders'] if r.get('Complete') and
            ((r['RuntimeName'] in names) if names else gpu.get(r['RuntimeName']) != 'OK')]
    WORK.mkdir(parents=True, exist_ok=True)
    prepared = []; results = {}
    for r in rows:
        try:
            text, subs = proxy(Path(r['WGSL']).read_text())
        except ValueError as error:
            results[r['RuntimeName']] = {'Status': 'PROXY-FAIL', 'Substitutions': [], 'WGSLSHA256': sha(r['WGSL']),
                                         'Detail': str(error)}
            continue
        path = WORK / f"{r['Name']}-proxy.wgsl"; path.write_text(text)
        prepared.append({'RuntimeName': r['RuntimeName'], 'Complete': True, 'WGSL': str(path),
                         'Shipped': r['WGSL'], 'Subs': subs})
    temp = WORK / 'proxy-manifest.json'
    temp.write_text(json.dumps({'Shaders': prepared}, indent=2))
    out = WORK / 'proxy-manifest-gpu-validation.json'
    if out.exists(): out.unlink()
    deno = next((str(p) for p in DENO_CANDIDATES if p.exists()), 'deno')
    if prepared:
        subprocess.run([deno, 'run', '-A', str(VALIDATOR), str(temp)], check=True)
        verdicts = json.loads(out.read_text())
    else:
        verdicts = {}
    for p in prepared:
        verdict = verdicts.get(p['RuntimeName'], 'not validated')
        results[p['RuntimeName']] = {'Status': 'PROXY-OK' if verdict == 'OK' else 'PROXY-FAIL',
                                     'Substitutions': p['Subs'], 'WGSLSHA256': sha(p['Shipped']),
                                     **({} if verdict == 'OK' else {'Detail': verdict})}
    existing = json.loads(PROXY.read_text()) if PROXY.exists() else {}
    shaders = {} if not names else dict(existing.get('Shaders', {}))
    shaders.update(results)
    PROXY.write_text(json.dumps({'Note': 'Headless Deno validation of a scratch copy of each WGSL with the listed substitutions (adapter compute limits). Shipped WGSL is unchanged. The package generators publish a row from here only when WGSLSHA256 matches and every substitution is covered by a runtime requiredCaps bit.', 'Tool': 'tools/headless/proxy-validate-compute.py',
                                 'Shaders': dict(sorted(shaders.items()))}, indent=2))
    ok = sum(v['Status'] == 'PROXY-OK' for v in results.values())
    print('proxy validated', len(results), 'ok', ok)
    for name, v in sorted(results.items()):
        if v['Status'] != 'PROXY-OK': print('  FAIL', name, v.get('Detail', '')[:200].replace('\n', ' '))


if __name__ == '__main__':
    main()
