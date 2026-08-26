from __future__ import annotations

import os
import subprocess
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from converter_core import (
    PRESETS,
    STABLE_PRESET,
    build_ffmpeg_audio_args,
    build_ffmpeg_video_args,
    build_index,
    find_ffmpeg,
    find_ffprobe,
    output_paths,
    parse_ffmpeg_time,
)

CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def probe_duration(path: Path, ffprobe: str) -> float:
    r = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", str(path)],
        text=True,
        capture_output=True,
        creationflags=CREATE_NO_WINDOW,
        check=True,
    )
    return max(0.0, float(r.stdout.strip() or 0.0))


def removable_video_dir() -> Path | None:
    if os.name != "nt":
        return None
    try:
        import ctypes
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        for i in range(26):
            if bitmask & (1 << i):
                root = f"{chr(65+i)}:\\"
                if ctypes.windll.kernel32.GetDriveTypeW(root) == 2:
                    p = Path(root) / "videos"
                    if p.exists() or os.access(root, os.W_OK):
                        return p
    except Exception:
        pass
    return None


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("CYD Video Converter - MP4 -> MJPEG + IDX")
        self.geometry("780x600")
        self.minsize(720, 520)

        self.input_var = tk.StringVar()
        default_out = removable_video_dir() or Path.cwd() / "converted"
        self.output_var = tk.StringVar(value=str(default_out))
        self.name_var = tk.StringVar()
        self.preset_var = tk.StringVar(value=STABLE_PRESET.name)
        self.audio_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Sẵn sàng")
        self.progress_var = tk.DoubleVar(value=0)
        self.cancel_event = threading.Event()
        self.worker: threading.Thread | None = None

        self._build_ui()

    def _build_ui(self):
        pad = {"padx": 12, "pady": 6}
        title = ttk.Label(self, text="CYD VIDEO CONVERTER", font=("Segoe UI", 18, "bold"))
        title.pack(pady=(14, 2))
        ttk.Label(self, text="MP4/MKV/AVI/WebM → MJPEG 320×240 + IDX tối ưu ESP32-2432S028R").pack()

        form = ttk.Frame(self)
        form.pack(fill="x", **pad)
        form.columnconfigure(1, weight=1)

        ttk.Label(form, text="Video nguồn").grid(row=0, column=0, sticky="w", padx=4, pady=6)
        ttk.Entry(form, textvariable=self.input_var).grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Button(form, text="Chọn video...", command=self.choose_input).grid(row=0, column=2, padx=4)

        ttk.Label(form, text="Thư mục xuất").grid(row=1, column=0, sticky="w", padx=4, pady=6)
        ttk.Entry(form, textvariable=self.output_var).grid(row=1, column=1, sticky="ew", padx=4)
        ttk.Button(form, text="Chọn thư mục...", command=self.choose_output).grid(row=1, column=2, padx=4)

        ttk.Label(form, text="Tên video").grid(row=2, column=0, sticky="w", padx=4, pady=6)
        ttk.Entry(form, textvariable=self.name_var).grid(row=2, column=1, sticky="ew", padx=4)
        ttk.Label(form, text="(.mjpeg / .idx)").grid(row=2, column=2, sticky="w", padx=4)

        ttk.Label(form, text="Preset").grid(row=3, column=0, sticky="w", padx=4, pady=6)
        preset_box = ttk.Combobox(form, textvariable=self.preset_var, values=list(PRESETS), state="readonly")
        preset_box.grid(row=3, column=1, sticky="ew", padx=4)
        preset_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh_preset_text())
        ttk.Checkbutton(form, text="Tạo MP3 (tùy chọn)", variable=self.audio_var).grid(row=3, column=2, sticky="w", padx=4)

        self.preset_desc = ttk.Label(form, text=STABLE_PRESET.description, foreground="#555")
        self.preset_desc.grid(row=4, column=1, columnspan=2, sticky="w", padx=4, pady=(0, 8))

        info = ttk.LabelFrame(self, text="Thiết lập khuyến nghị cho CYD")
        info.pack(fill="x", **pad)
        ttk.Label(
            info,
            text="Mặc định: 320×240 · 12 fps · MJPEG JPEG q=10 · yuvj420p · IDX CYD1. "
                 "Ưu tiên ổn định SD + thời gian giải mã hơn FPS cao.",
            wraplength=720,
        ).pack(anchor="w", padx=10, pady=8)

        progress_frame = ttk.Frame(self)
        progress_frame.pack(fill="x", **pad)
        ttk.Progressbar(progress_frame, variable=self.progress_var, maximum=100).pack(fill="x")
        ttk.Label(progress_frame, textvariable=self.status_var).pack(anchor="w", pady=(4, 0))

        buttons = ttk.Frame(self)
        buttons.pack(fill="x", **pad)
        self.convert_btn = ttk.Button(buttons, text="CHUYỂN VIDEO", command=self.start_convert)
        self.convert_btn.pack(side="left")
        self.cancel_btn = ttk.Button(buttons, text="Hủy", command=self.cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=8)
        ttk.Button(buttons, text="Mở thư mục xuất", command=self.open_output).pack(side="right")

        log_frame = ttk.LabelFrame(self, text="Nhật ký")
        log_frame.pack(fill="both", expand=True, **pad)
        self.log = tk.Text(log_frame, height=12, wrap="word", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=6, pady=6)

    def choose_input(self):
        f = filedialog.askopenfilename(
            title="Chọn video",
            filetypes=[("Video", "*.mp4 *.mkv *.avi *.webm *.mov *.m4v"), ("Tất cả", "*.*")],
        )
        if f:
            self.input_var.set(f)
            self.name_var.set(Path(f).stem)

    def choose_output(self):
        d = filedialog.askdirectory(title="Chọn thư mục xuất")
        if d:
            self.output_var.set(d)

    def refresh_preset_text(self):
        self.preset_desc.config(text=PRESETS[self.preset_var.get()].description)

    def append_log(self, text: str):
        self.after(0, lambda: (self.log.insert("end", text.rstrip() + "\n"), self.log.see("end")))

    def set_status(self, text: str, progress: float | None = None):
        def update():
            self.status_var.set(text)
            if progress is not None:
                self.progress_var.set(max(0, min(100, progress)))
        self.after(0, update)

    def start_convert(self):
        if self.worker and self.worker.is_alive():
            return
        src = Path(self.input_var.get().strip())
        if not src.is_file():
            messagebox.showerror("Lỗi", "Hãy chọn một file video nguồn hợp lệ.")
            return
        out = Path(self.output_var.get().strip())
        name = self.name_var.get().strip() or src.stem
        preset = PRESETS[self.preset_var.get()]
        self.cancel_event.clear()
        self.convert_btn.config(state="disabled")
        self.cancel_btn.config(state="normal")
        self.log.delete("1.0", "end")
        self.worker = threading.Thread(target=self._convert_worker, args=(src, out, name, preset), daemon=True)
        self.worker.start()

    def _run_ffmpeg_progress(self, args: list[str], duration: float, base: float, span: float):
        args = list(args)
        args[1:1] = ["-loglevel", "warning", "-progress", "pipe:1", "-nostats"]
        self.append_log("$ " + " ".join(f'"{x}"' if " " in str(x) else str(x) for x in args))
        p = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=CREATE_NO_WINDOW,
        )
        assert p.stdout is not None
        for line in p.stdout:
            if self.cancel_event.is_set():
                p.terminate()
                raise RuntimeError("Đã hủy bởi người dùng")
            sec = parse_ffmpeg_time(line.strip())
            if sec is not None and duration > 0:
                pct = base + min(sec / duration, 1.0) * span
                self.set_status(f"Đang chuyển video… {sec:.1f}/{duration:.1f} giây", pct)
            elif line.strip() and not line.startswith(("frame=", "fps=", "stream_", "bitrate=", "total_size=", "out_time", "dup_frames=", "drop_frames=", "speed=", "progress=")):
                self.append_log(line)
        rc = p.wait()
        if rc != 0:
            raise RuntimeError(f"FFmpeg thất bại, mã lỗi {rc}")

    def _convert_worker(self, src: Path, out: Path, name: str, preset):
        started = time.time()
        try:
            ffmpeg = find_ffmpeg()
            ffprobe = find_ffprobe(ffmpeg)
            out.mkdir(parents=True, exist_ok=True)
            mjpeg, idx, mp3 = output_paths(out, name)
            duration = probe_duration(src, ffprobe)
            self.append_log(f"Nguồn: {src}")
            self.append_log(f"Thời lượng: {duration:.2f} s")
            self.append_log(f"Preset: {preset.name} - {preset.width}x{preset.height} @ {preset.fps:g} fps, q={preset.quality}")
            self.set_status("Đang chuyển video sang MJPEG…", 1)
            self._run_ffmpeg_progress(build_ffmpeg_video_args(src, mjpeg, preset, ffmpeg), duration, 1, 88)

            if self.audio_var.get() and not self.cancel_event.is_set():
                self.set_status("Đang tạo MP3…", 90)
                self._run_ffmpeg_progress(build_ffmpeg_audio_args(src, mp3, ffmpeg), duration, 90, 6)

            if self.cancel_event.is_set():
                raise RuntimeError("Đã hủy bởi người dùng")

            self.set_status("Đang kiểm tra JPEG và tạo IDX…", 97)
            info = build_index(mjpeg, idx, preset.fps)
            elapsed = time.time() - started
            self.append_log(
                f"VERIFY OK: {info.frames} frame, {info.duration:.1f}s, "
                f"avg={info.avg_frame_bytes/1024:.1f} KB/frame, max={info.max_frame_bytes/1024:.1f} KB"
            )
            self.append_log(f"MJPEG: {mjpeg} ({mjpeg.stat().st_size/1048576:.1f} MB)")
            self.append_log(f"IDX:   {idx} ({idx.stat().st_size/1024:.1f} KB)")
            if self.audio_var.get() and mp3.exists():
                self.append_log(f"MP3:   {mp3} ({mp3.stat().st_size/1048576:.1f} MB)")
            self.set_status(f"HOÀN TẤT · {elapsed:.1f}s · {info.frames} frame", 100)
            self.after(0, lambda: messagebox.showinfo("Hoàn tất", f"Đã tạo file cho CYD:\n{mjpeg.name}\n{idx.name}"))
        except Exception as exc:
            self.append_log(f"ERROR: {exc}")
            self.set_status(f"Lỗi: {exc}")
            self.after(0, lambda e=str(exc): messagebox.showerror("Lỗi chuyển đổi", e))
        finally:
            self.after(0, lambda: (self.convert_btn.config(state="normal"), self.cancel_btn.config(state="disabled")))

    def cancel(self):
        self.cancel_event.set()
        self.status_var.set("Đang hủy…")

    def open_output(self):
        p = Path(self.output_var.get().strip())
        p.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            os.startfile(p)


if __name__ == "__main__":
    App().mainloop()