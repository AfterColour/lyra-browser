# Draws the Lyra icon (a handwritten L traced by a constellation) and writes
#   src\Lyra.ico          every size Windows asks for (16-256 px), used by Lyra.exe
#   art\lyra-icon-<n>.png 256 / 512 / 1024 px copies for the installer and the website
#   dev\icon-preview.png  all sizes on light and dark backgrounds
# Small sizes are drawn separately (fewer stars, thicker stroke), not just scaled down.
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Drawing2D; using System.Drawing.Imaging;
public static class LyraIcon {
    // The stroke of a script L in a 256 x 256 design space (a cardinal spline through these
    // points): a loop at the top, the stem leaning down to a small loop at the foot, then a
    // long sweep to the right.
    static readonly float[,] Path = {
        {134,110}, {156,76}, {166,48}, {152,32}, {130,36}, {116,60}, {108,100}, {100,142},
        {92,180}, {80,204}, {60,218}, {46,208}, {56,194}, {82,198}, {120,210}, {158,212}, {200,192} };
    // Stars sit on points of the stroke: {point, size, kind (0 star, 1 sparkle, 2 the gold star)}
    static readonly int[,] Stars = { {2,110,1}, {7,85,0}, {11,90,0}, {15,85,0}, {16,105,2} };
    static readonly int[,] SmallStars = { {2,100,0}, {11,95,0}, {16,110,2} };
    static readonly float[,] Dust = { {186,70},{214,104},{170,118},{220,146},{58,64},{192,140},{40,236},{228,50},{150,238},{40,120},{120,240},{226,228} };

    static GraphicsPath Rounded(float x, float y, float w, float h, float r) {
        var p = new GraphicsPath(); float d = r * 2;
        p.AddArc(x, y, d, d, 180, 90); p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90); p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure(); return p;
    }

    static void Glow(Graphics g, float cx, float cy, float r, Color c, int alpha) {
        using (var gp = new GraphicsPath()) {
            gp.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
            using (var pb = new PathGradientBrush(gp)) {
                pb.CenterColor = Color.FromArgb(alpha, c);
                pb.SurroundColors = new[] { Color.FromArgb(0, c) };
                g.FillPath(pb, gp);
            }
        }
    }

    static void Sparkle(Graphics g, float cx, float cy, float L, float w, Color c, int alpha) {
        using (var sp = new GraphicsPath()) {
            sp.AddPolygon(new[] { new PointF(cx, cy - L), new PointF(cx + w, cy - w), new PointF(cx + L, cy),
                new PointF(cx + w, cy + w), new PointF(cx, cy + L), new PointF(cx - w, cy + w),
                new PointF(cx - L, cy), new PointF(cx - w, cy - w) });
            using (var b = new SolidBrush(Color.FromArgb(alpha, c))) g.FillPath(b, sp);
        }
    }

    public static Bitmap Draw(int n) {
        var bmp = new Bitmap(n, n, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.Clear(Color.Transparent);
            bool tiny = n <= 48, big = n >= 64;
            float s = n / 256f;
            float m = tiny ? 0 : 8 * s;                       // margin around the tile
            var tile = Rounded(m, m, n - 2 * m, n - 2 * m, (tiny ? 44 : 52) * s);
            using (var bg = new LinearGradientBrush(new PointF(0, 0), new PointF(n, n),
                       Color.FromArgb(255, 22, 18, 58), Color.FromArgb(255, 58, 30, 104)))
                g.FillPath(bg, tile);
            g.SetClip(tile);
            if (!tiny) Glow(g, 120 * s, 160 * s, 120 * s, Color.FromArgb(120, 90, 230), 100);   // nebula
            if (big) {     // faint background stars
                for (int i = 0; i < Dust.GetLength(0); i++) {
                    float r = (i % 3 == 0 ? 2.5f : 1.7f) * s;
                    using (var b = new SolidBrush(Color.FromArgb(i % 2 == 0 ? 90 : 55, 220, 215, 255)))
                        g.FillEllipse(b, Dust[i, 0] * s - r, Dust[i, 1] * s - r, 2 * r, 2 * r);
                }
            }
            // The stroke of the letter: a soft glow and a fine line of starlight
            var pts = new PointF[Path.GetLength(0)];
            for (int i = 0; i < pts.Length; i++) pts[i] = new PointF(Path[i, 0] * s, Path[i, 1] * s);
            using (var stroke = new GraphicsPath()) {
                stroke.AddCurve(pts, 0.55f);
                Color line = Color.FromArgb(205, 195, 255);
                if (!tiny) {
                    using (var pen = new Pen(Color.FromArgb(55, line), 11 * s)) { pen.StartCap = pen.EndCap = LineCap.Round; pen.LineJoin = LineJoin.Round; g.DrawPath(pen, stroke); }
                }
                float lw = tiny ? Math.Max(1.5f, 15 * s) : Math.Max(1f, 3.6f * s);
                using (var pen = new Pen(Color.FromArgb(tiny ? 240 : 200, line), lw)) {
                    pen.StartCap = pen.EndCap = LineCap.Round; pen.LineJoin = LineJoin.Round;
                    g.DrawPath(pen, stroke);
                }
            }
            // Stars on the stroke
            var stars = tiny ? SmallStars : Stars;
            for (int k = 0; k < stars.GetLength(0); k++) {
                float cx = Path[stars[k, 0], 0] * s, cy = Path[stars[k, 0], 1] * s;
                int kind = stars[k, 2];
                float core = (tiny ? 21 : 9.5f) * stars[k, 1] / 100f * s;
                if (tiny) core = Math.Max(core, 1.6f);
                Color c = kind == 2 ? Color.FromArgb(255, 255, 214, 140) : Color.FromArgb(255, 245, 242, 255);
                if (!tiny) {
                    Glow(g, cx, cy, core * 3.2f, c, kind == 2 ? 170 : 120);
                    if (kind > 0) Sparkle(g, cx, cy, core * (kind == 2 ? 3.4f : 3.0f), core * 0.42f, c, kind == 2 ? 235 : 210);
                }
                using (var b = new SolidBrush(c)) g.FillEllipse(b, cx - core, cy - core, 2 * core, 2 * core);
            }
            g.ResetClip();
            if (!tiny) using (var edge = new Pen(Color.FromArgb(60, 190, 170, 255), Math.Max(1f, 1.5f * s))) g.DrawPath(edge, tile);
        }
        return bmp;
    }
}
"@
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = foreach ($n in $sizes) {
    $b = [LyraIcon]::Draw($n); $ms = New-Object IO.MemoryStream
    $b.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); ,$ms.ToArray()
}
# .ico = header + directory + PNG images
$out = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $n = $sizes[$i]; $d = if ($n -ge 256) { 0 } else { $n }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
[IO.File]::WriteAllBytes((Join-Path $root "src\Lyra.ico"), $out.ToArray())
New-Item -ItemType Directory -Force (Join-Path $root "art") | Out-Null
foreach ($n in 256, 512, 1024) {
    $b = [LyraIcon]::Draw($n); $b.Save((Join-Path $root "art\lyra-icon-$n.png"), [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}
# Preview sheet: every size side by side on light and dark backgrounds
New-Item -ItemType Directory -Force (Join-Path $root "dev") | Out-Null
$sheet = New-Object Drawing.Bitmap 900, 420; $g = [Drawing.Graphics]::FromImage($sheet)
$g.FillRectangle([Drawing.Brushes]::WhiteSmoke, 0, 0, 900, 210); $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(32, 32, 36))), 0, 210, 900, 210)
foreach ($row in 0, 1) { $x = 12; foreach ($n in $sizes) { $b = [LyraIcon]::Draw($n); $g.DrawImage($b, $x, 12 + $row * 210 + (192 - [Math]::Min($n, 192)) / 2, [Math]::Min($n, 192), [Math]::Min($n, 192)); $x += [Math]::Min($n, 192) + 14; $b.Dispose() } }
$sheet.Save((Join-Path $root "dev\icon-preview.png"), [Drawing.Imaging.ImageFormat]::Png)
"icon written: src\Lyra.ico ($($out.Length) bytes)"
