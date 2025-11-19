import os
import glob
import re
from mido import Message, MidiFile, MidiTrack, bpm2tempo

# ========= 参数 =========
BPM = 120
PPQ = 480        # ticks per quarter note
DUR_TICKS = PPQ  # 每个音符 = 四分音符

def freq_to_midi(freq):
    """freq(Hz) -> MIDI note number (int)"""
    if freq <= 0:
        return None
    import math
    midi = 69 + 12 * math.log2(freq / 440.0)
    n = int(round(midi))
    if n < 0 or n > 127:
        return None
    return n

def parse_freqs_from_line(line):
    """从一行 log 里挖出所有 Hz 数字，返回 [float, float, ...]"""
    matches = re.findall(r'([0-9]+\.[0-9]+|[0-9]+)\s*Hz', line)
    return [float(m) for m in matches]

def find_latest_txt():
    """在 savedstuff/ 下找最新的 txt 文件"""
    base_dir = os.path.dirname(os.path.abspath(__file__))
    saved_dir = os.path.join(base_dir, "savedstuff")

    pattern = os.path.join(saved_dir, "*.txt")
    files = glob.glob(pattern)
    if not files:
        print("❌ 没有在 savedstuff/ 里找到任何 .txt 文件")
        return None

    latest = max(files, key=os.path.getmtime)
    print(f"✅ 使用最新的 txt 文件: {latest}")
    return latest

def main():
    log_file = "/Users/yifengyuan/Documents/AudioPluginPeaks.txt"
    if log_file is None:
        return

    base_dir = os.path.dirname(os.path.abspath(__file__))
    out_midi = "output.mid"

    mid = MidiFile()
    mid.ticks_per_beat = PPQ

    track = MidiTrack()
    mid.tracks.append(track)

    tempo = bpm2tempo(BPM)
    track.append(Message('program_change', program=0, time=0))

    current_time = 0

    with open(log_file, 'r', encoding='utf-8') as f:
        for line in f:
            freqs = parse_freqs_from_line(line)
            if not freqs:
                continue

            notes = []
            for freq in freqs:
                midi_note = freq_to_midi(freq)
                if midi_note is not None:
                    notes.append(midi_note)

            if not notes:
                continue

            # 把这一行的所有 peak 当成“一个和弦”，同时开始、同时结束
            for i, n in enumerate(notes):
                time = current_time if i == 0 else 0
                track.append(Message('note_on', note=n, velocity=80, time=time))

            for i, n in enumerate(notes):
                time = DUR_TICKS if i == 0 else 0
                track.append(Message('note_off', note=n, velocity=0, time=time))

            current_time = 0  # 已经用 time 体现时值，不再累计

    mid.save(out_midi)
    print(f"🎵 已生成 MIDI: {out_midi}")

if __name__ == '__main__':
    main()
