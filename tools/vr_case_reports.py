"""One autopsy report per Naomi case, made from the game's own files (the case briefing + the victim's
portrait from the case-briefing screen). Output: vr/case_report_<stage>.png (mirrored for the desk paper's UVs).
    python case_reports.py <game files dir> <textures dir (extracted pngs)> <out dir>"""
import sys, os, glob, textwrap
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scripts import msgs_json

# stage -> (briefing comic, the lines that state the facts, victim, description, portrait sprite)
CASES = {
    '110': ('510_020', ['0030', '0040', '0050', '0060', '0070'], 'Dennis Taylor', 'Male, 30', 'kenshi_pic01'),
    '120': ('520_020', ['0030', '0040', '0050', '0060', '0080'], 'Unidentified', 'Female (suspected), skeletal remains', 'kenshi_pic02'),
    '130': ('530_020', ['0020', '0030', '0040', '0050', '0060'], 'Alma Parker', 'Female, 75', 'kenshi_pic03'),
    '140': ('540_020', ['0050', '0070', '0080', '0110', '0170', '0180'], 'Aidan Posner', 'Male, tennis player', 'kenshi_pic04'),
    '150': ('550_020', ['0040', '0050', '0080'], 'Stephen Eldred', '"The Revolutionary"', 'kenshi_pic05'),
    '210': ('760_020', ['0140', '0150', '0180', '0200'], 'Unidentified', 'Skeletal remains', None),
    '220': ('790_020', ['0040', '0050', '0070', '0080'], 'Rosalia Rossellini', 'Female', 'kenshi_pic07'),
}
FONTS = '/root/trailer/fonts/'

def clean(t): return ' '.join(t.replace('\n', ' ').split()).replace('- ', '-').replace('  ', ' ')

def make(stage, files, tex, out):
    comic, keys, name, desc, pic = CASES[stage]
    ms = {m['name']: clean(' '.join(p['text'] for p in m['pages'])) for m in msgs_json(open(os.path.join(files, 'comic/msg/eng', comic + '.bm2'), 'rb').read())}
    lines = [ms['MSG_%s_%s' % (comic, k)] for k in keys if 'MSG_%s_%s' % (comic, k) in ms]
    W, H = 1200, 1600
    im = Image.new('RGB', (W, H), (238, 236, 229)); d = ImageDraw.Draw(im)
    F = lambda n, s: ImageFont.truetype(FONTS + n, s)
    fb, fh, fm, fs = F('ChakraPetch-Bold.ttf', 64), F('ChakraPetch-Bold.ttf', 36), F('IBMPlexMono-Medium.ttf', 30), F('IBMPlexMono-Medium.ttf', 24)
    d.text((80, 70), 'AUTOPSY REPORT', font=fb, fill=(25, 25, 30))
    d.text((82, 150), 'Cumberland Institute of Forensic Medicine  -  case ' + stage, font=fs, fill=(90, 90, 95))
    d.line((80, 200, W - 80, 200), fill=(40, 40, 45), width=4)
    if pic:
        f = glob.glob(os.path.join(tex, 'sprite/conf', pic + '.tpx', '*.png'))
        if f:
            ph = Image.open(f[0]).convert('RGB').resize((340, 340), Image.LANCZOS)
            im.paste((255, 255, 255), (W - 80 - 354, 226, W - 66, 620)); im.paste(ph, (W - 80 - 340, 240))
            d.text((W - 80 - 340, 586), 'Photo attached', font=fs, fill=(100, 100, 105))
            d.rectangle((W - 280, 226, W - 220, 252), fill=(200, 200, 195))
    y = 250
    for k, v in [('NAME', name), ('DETAILS', desc), ('EXAMINER', 'Dr. Naomi Kimishima')]:
        d.text((80, y), k, font=fs, fill=(110, 110, 115))
        for ln in textwrap.wrap(v, 30): d.text((80, y + 30), ln, font=fm, fill=(25, 25, 30)); y += 40
        y += 50
    y = max(y, 650)
    d.line((80, y, W - 80, y), fill=(40, 40, 45), width=2); y += 30
    d.text((80, y), 'CASE BRIEFING', font=fh, fill=(25, 25, 30)); y += 60
    for t in lines:
        for ln in textwrap.wrap(t, 58): d.text((80, y), ln, font=fm, fill=(35, 35, 40)); y += 42
        y += 14
    im = im.transpose(Image.FLIP_TOP_BOTTOM)   # the desk paper's UVs
    im.save(os.path.join(out, 'case_report_%s.png' % stage))

if __name__ == '__main__':
    files, tex, out = sys.argv[1:4]; os.makedirs(out, exist_ok=True)
    for st in CASES: make(st, files, tex, out); print('case', st)
