"""Apply the bounded text-renderer fix only to the verified LVGL source."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def apply():
    spec=json.loads((ROOT/'firmware/patches/lvgl.json').read_text())
    target=ROOT/'.local/sdk/esp32/managed_components/lvgl__lvgl'/spec['file']
    if not target.exists(): raise SystemExit('Resolve ESP-IDF components first; see docs/BUILD.md.')
    raw=target.read_bytes().replace(b'\r\n',b'\n'); current=hashlib.sha256(raw).hexdigest()
    if current==spec['after_sha256']: return
    if current!=spec['before_sha256']: raise SystemExit('Unknown LVGL version: review the renderer patch before building.')
    text=raw.decode()
    needle='            lv_text_encoded_letter_next_2(bidi_txt, &letter, &letter_next, &next_char_offset);'
    replacement='''            uint32_t previous_char_offset = next_char_offset;
            lv_text_encoded_letter_next_2(bidi_txt, &letter, &letter_next, &next_char_offset);
            /* A stale window length or truncated UTF-8 must never strand the
             * drawing worker at a terminator while the UI waits for it. */
            if(letter == 0 || next_char_offset <= previous_char_offset) break;'''
    assert text.count(needle)==1
    text=text.replace(needle,replacement).replace('        line_start = line_end;\n        if(remaining_len) {','        if(line_end <= line_start) break;\n        line_start = line_end;\n        if(remaining_len) {')
    result=text.encode()
    if hashlib.sha256(result).hexdigest()!=spec['after_sha256']: raise SystemExit('Renderer patch integrity mismatch.')
    target.write_bytes(result)
    print('Applied verified LVGL forward-progress fix.')

if __name__=='__main__': apply()
