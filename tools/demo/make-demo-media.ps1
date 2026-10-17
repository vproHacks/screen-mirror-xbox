# Generates the media used for the Store screenshots (see capture-screenshots.sh):
#   build/demo/canon.wav  Pachelbel's Canon in D (public domain), synthesized here and tagged
#   build/demo/cover.jpg  abstract cover art, drawn here
# Everything is generated, so the screenshots contain no third-party recordings or artwork.
# Usage (from Git Bash): powershell -NoProfile -ExecutionPolicy Bypass -File tools/demo/make-demo-media.ps1
param([string]$Out = (Join-Path $PSScriptRoot "..\..\build\demo"))
$ErrorActionPreference = "Stop"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

public static class DemoMedia
{
    const int Rate = 44100;
    const double Beat = 1.0;  // seconds per bass note
    const int Cycles = 18;    // the 8-note ground bass is played this many times, then a final chord

    // ground bass D A B F# G D G A, and the chord above each note
    static readonly int[] Bass = { 50, 45, 47, 42, 43, 38, 43, 45 };
    static readonly int[][] Chords = {
        new[] { 62, 66, 69 }, new[] { 61, 64, 69 }, new[] { 62, 66, 71 }, new[] { 61, 66, 69 },
        new[] { 62, 67, 71 }, new[] { 62, 66, 69 }, new[] { 62, 67, 71 }, new[] { 61, 64, 69 } };

    // the melodic lines each voice plays in turn, one per 8-beat cycle; the voices enter two
    // cycles apart (a canon), so every line sounds against the others
    static readonly int[] Line1 = { 78, 76, 74, 73, 71, 69, 71, 73 };
    static readonly int[] Line2 = { 74, 73, 71, 69, 67, 66, 67, 64 };
    static readonly int[] Line3 = { 74, 78, 81, 79, 78, 74, 78, 76, 74, 71, 74, 81, 79, 83, 81, 79 };

    static double[] VoiceHarm = { 1.0, 0.45, 0.2, 0.1, 0.05 };
    static double[] BassHarm = { 1.0, 0.5, 0.25 };
    static double[] PadHarm = { 1.0, 0.3, 0.15 };

    static void Note(float[] L, float[] R, double start, double dur, int midi, double gain, double pan,
                     double[] harm, double decay, double attack, double release)
    {
        double f = 440.0 * Math.Pow(2, (midi - 69) / 12.0);
        double gl = Math.Cos(pan * Math.PI / 2), gr = Math.Sin(pan * Math.PI / 2);
        int s0 = (int) (start * Rate);
        int n = (int) ((dur + release * 5) * Rate);
        for (int i = 0; i < n && s0 + i < L.Length; i++)
        {
            double t = (double) i / Rate;
            double env = t < attack ? t / attack : 1.0;
            if (t > dur) env *= Math.Exp(-(t - dur) / release);
            double v = 0;
            for (int h = 0; h < harm.Length; h++)
            {
                double fh = f * (h + 1);
                if (fh > 16000) break;
                v += harm[h] * Math.Exp(-decay * (1 + 0.6 * h) * t) * Math.Sin(2 * Math.PI * fh * t);
            }
            v *= env * gain;
            L[s0 + i] += (float) (v * gl);
            R[s0 + i] += (float) (v * gr);
        }
    }

    // one cycle of line `index` (0..5) for a voice, starting at time t0
    static void PlayLine(float[] L, float[] R, int index, double t0, double gain, double pan)
    {
        switch (index)
        {
        case 0:
        case 1:
            int[] line = index == 0 ? Line1 : Line2;
            for (int b = 0; b < 8; b++) Voice(L, R, t0 + b * Beat, Beat, line[b], gain, pan);
            break;
        case 2:
            for (int i = 0; i < 16; i++) Voice(L, R, t0 + i * Beat / 2, Beat / 2, Line3[i], gain, pan);
            break;
        case 3:  // rising broken chords
        case 4:  // falling broken chords
            for (int b = 0; b < 8; b++)
            {
                int[] c = Chords[b];
                int[] pattern = index == 3 ? new[] { c[0], c[1], c[2], c[1] } : new[] { c[2], c[1], c[0], c[1] };
                for (int i = 0; i < 4; i++) Voice(L, R, t0 + b * Beat + i * Beat / 4, Beat / 4, pattern[i] + 12, gain * 0.8, pan);
            }
            break;
        default:  // the first two lines interleaved, in thirds and sixths
            for (int b = 0; b < 8; b++)
            {
                Voice(L, R, t0 + b * Beat, Beat / 2, Line1[b], gain, pan);
                Voice(L, R, t0 + b * Beat + Beat / 2, Beat / 2, Line2[b], gain, pan);
            }
            break;
        }
    }

    static void Voice(float[] L, float[] R, double start, double dur, int midi, double gain, double pan)
    {
        Note(L, R, start, dur, midi, gain, pan, VoiceHarm, 1.6, 0.004, 0.22);
    }

    // Freeverb-style room: parallel damped combs, then allpasses
    static float[] Reverb(float[] x, int spread)
    {
        int[] combs = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        int[] allpasses = { 556, 441, 341, 225 };
        float[] y = new float[x.Length];
        foreach (int d0 in combs)
        {
            int d = d0 + spread, idx = 0;
            float[] buf = new float[d];
            float lp = 0;
            for (int i = 0; i < x.Length; i++)
            {
                float o = buf[idx];
                lp = o * 0.75f + lp * 0.25f;
                buf[idx] = x[i] + lp * 0.84f;
                idx = (idx + 1) % d;
                y[i] += o;
            }
        }
        foreach (int d0 in allpasses)
        {
            int d = d0 + spread, idx = 0;
            float[] buf = new float[d];
            for (int i = 0; i < y.Length; i++)
            {
                float b = buf[idx];
                buf[idx] = y[i] + b * 0.5f;
                y[i] = b - y[i];
                idx = (idx + 1) % d;
            }
        }
        return y;
    }

    static double Rms(float[] x)
    {
        double s = 0;
        foreach (float v in x) s += v * v;
        return Math.Sqrt(s / x.Length);
    }

    public static string WriteSong(string path)
    {
        double cycle = 8 * Beat;
        double end = Cycles * cycle;
        double length = end + 7.0;
        int n = (int) (length * Rate);
        float[] L = new float[n], R = new float[n];

        for (int c = 0; c < Cycles; c++)
        {
            double t0 = c * cycle;
            for (int b = 0; b < 8; b++)
            {
                double t = t0 + b * Beat;
                Note(L, R, t, Beat, Bass[b], 0.32, 0.5, BassHarm, 0.9, 0.006, 0.12);
                if (c >= 1)
                    for (int i = 0; i < 3; i++)
                        Note(L, R, t, Beat, Chords[b][i], 0.045, 0.35 + 0.15 * i, PadHarm, 0.25, 0.18, 0.25);
            }
            int[] entries = { 2, 4, 6 };
            double[] pans = { 0.3, 0.72, 0.5 };
            for (int v = 0; v < 3; v++)
                if (c >= entries[v] && c < Cycles) PlayLine(L, R, (c - entries[v]) % 6, t0, 0.15, pans[v]);
        }
        // final D major chord
        foreach (int m in new[] { 38, 50, 57, 62, 66, 69, 74, 78 })
            Note(L, R, end, 4.0, m, m < 55 ? 0.28 : 0.11, 0.5, m < 55 ? BassHarm : VoiceHarm, 0.45, 0.01, 1.2);

        // room reverb at about a third of the dry level
        float[] wl = Reverb(L, 0), wr = Reverb(R, 23);
        double wet = 0.3 * Rms(L) / Math.Max(1e-9, Rms(wl));
        for (int i = 0; i < n; i++)
        {
            L[i] += (float) (wl[i] * wet);
            R[i] += (float) (wr[i] * wet);
        }
        // fade in briefly, fade out over the last seconds, normalize to -1 dBFS
        float peak = 0;
        for (int i = 0; i < n; i++)
        {
            double t = (double) i / Rate;
            double g = Math.Min(1.0, t / 0.3) * Math.Min(1.0, Math.Max(0.0, (length - t) / 4.0));
            L[i] *= (float) g;
            R[i] *= (float) g;
            peak = Math.Max(peak, Math.Max(Math.Abs(L[i]), Math.Abs(R[i])));
        }
        float scale = 0.89f / peak;

        using (var w = new BinaryWriter(File.Create(path)))
        {
            byte[] info = InfoChunk(new[] {
                "INAM", "Canon in D",
                "IART", "Johann Pachelbel",
                "IPRD", "Canon and Gigue in D major",
                "ICMT", "Synthesized for the Screen Mirror Server screenshots; public domain (CC0)" });
            int dataBytes = n * 4;
            w.Write(Encoding.ASCII.GetBytes("RIFF"));
            w.Write(4 + (8 + 16) + info.Length + (8 + dataBytes));
            w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
            w.Write(16);
            w.Write((short) 1);
            w.Write((short) 2);
            w.Write(Rate);
            w.Write(Rate * 4);
            w.Write((short) 4);
            w.Write((short) 16);
            w.Write(info);
            w.Write(Encoding.ASCII.GetBytes("data"));
            w.Write(dataBytes);
            for (int i = 0; i < n; i++)
            {
                w.Write((short) Math.Round(L[i] * scale * 32767));
                w.Write((short) Math.Round(R[i] * scale * 32767));
            }
        }
        return string.Format("{0:F1} s, peak before normalizing {1:F2}, reverb gain {2:F3}", length, peak, wet);
    }

    static byte[] InfoChunk(string[] tags)
    {
        var body = new MemoryStream();
        var bw = new BinaryWriter(body);
        bw.Write(Encoding.ASCII.GetBytes("INFO"));
        for (int i = 0; i < tags.Length; i += 2)
        {
            byte[] v = Encoding.UTF8.GetBytes(tags[i + 1] + "\0");
            bw.Write(Encoding.ASCII.GetBytes(tags[i]));
            bw.Write(v.Length);
            bw.Write(v);
            if (v.Length % 2 == 1) bw.Write((byte) 0);
        }
        bw.Flush();
        var chunk = new MemoryStream();
        var cw = new BinaryWriter(chunk);
        cw.Write(Encoding.ASCII.GetBytes("LIST"));
        cw.Write((int) body.Length);
        cw.Write(body.ToArray());
        cw.Flush();
        return chunk.ToArray();
    }

    // dusk gradient, a low sun, flowing lines like a stave, a few soft lights
    public static void WriteCover(string path)
    {
        const int S = 1000;
        using (var bmp = new Bitmap(S, S))
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var bg = new LinearGradientBrush(new Rectangle(0, 0, S, S), Color.Black, Color.Black, 90f))
            {
                var blend = new ColorBlend(4);
                blend.Colors = new[] { Color.FromArgb(16, 22, 64), Color.FromArgb(70, 40, 112), Color.FromArgb(196, 88, 104),
                                       Color.FromArgb(246, 168, 92) };
                blend.Positions = new[] { 0f, 0.42f, 0.72f, 1f };
                bg.InterpolationColors = blend;
                g.FillRectangle(bg, 0, 0, S, S);
            }
            using (var sun = new GraphicsPath())
            {
                sun.AddEllipse(S * 0.5f - 330, S * 0.7f - 330, 660, 660);
                using (var glow = new PathGradientBrush(sun))
                {
                    glow.CenterColor = Color.FromArgb(200, 255, 226, 170);
                    glow.SurroundColors = new[] { Color.FromArgb(0, 255, 200, 140) };
                    g.FillPath(glow, sun);
                }
            }
            using (var disc = new SolidBrush(Color.FromArgb(235, 255, 236, 200)))
                g.FillEllipse(disc, S * 0.5f - 95, S * 0.7f - 95, 190, 190);
            for (int k = 0; k < 9; k++)
            {
                var pts = new PointF[121];
                for (int i = 0; i <= 120; i++)
                {
                    float x = i * S / 120f;
                    double phase = x / S * 2 * Math.PI * 1.15 + k * 0.32;
                    pts[i] = new PointF(x, S * 0.6f + k * 24 + (float) (70 * Math.Sin(phase) * (1 - k * 0.05)));
                }
                using (var pen = new Pen(Color.FromArgb(60 + k * 16, 255, 240, 220), 3.2f)) g.DrawCurve(pen, pts);
            }
            var rnd = new Random(7);
            for (int i = 0; i < 46; i++)
            {
                float r = 4 + (float) rnd.NextDouble() * 22;
                float x = (float) rnd.NextDouble() * S, y = (float) rnd.NextDouble() * S * 0.5f;
                using (var dot = new SolidBrush(Color.FromArgb(18 + rnd.Next(50), 255, 246, 230)))
                    g.FillEllipse(dot, x - r, y - r, 2 * r, 2 * r);
            }
            ImageCodecInfo jpeg = Array.Find(ImageCodecInfo.GetImageEncoders(), e => e.FormatID == ImageFormat.Jpeg.Guid);
            var p = new EncoderParameters(1);
            p.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 92L);
            bmp.Save(path, jpeg, p);
        }
    }
}
'@

New-Item -ItemType Directory -Force $Out | Out-Null
$song = Join-Path $Out "canon.wav"
$cover = Join-Path $Out "cover.jpg"
"canon.wav: " + [DemoMedia]::WriteSong($song)
[DemoMedia]::WriteCover($cover)
"cover.jpg written"
