<#
  Cisco's OpenH264 binary for software decoding (clients without a usable NVIDIA GPU) and CPU encoding.
  Dot-sourced by install.ps1 and get-openh264.ps1; defines Install-OpenH264 and the pinned hashes.
  Cisco pays the H.264 royalties only for its own binary downloaded by the end user, who must be
  able to turn it off (install.ps1 -SkipOpenH264 now, deleting the DLL later); the notice below is required.
  When upgrading OpenH264, change the URL and both hashes together.
#>
$OpenH264Dll ='openh264-2.7.0-win64.dll'
$OpenH264Url = "https://ciscobinary.openh264.org/$OpenH264Dll.bz2"
$OpenH264Bz2Sha256 = '7073fda55e904ac515f787d9728c4bcf9965ec7c91c9f84eae3f260ef9b9b37c'
$OpenH264DllSha256 = '0ef5e7f3b38753b15c13afa0418d4d7805fa228e6adfd3e425bbf758cff1c288'

function Install-OpenH264 {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin)
    Write-Host 'OpenH264 Video Codec provided by Cisco Systems, Inc. (licenses\openh264\BINARY_LICENSE.txt)'
    if (-not $PSCmdlet.ShouldProcess((Join-Path $Bin $OpenH264Dll), "Download $OpenH264Url and check sha256")) { return }
    $work = Join-Path ([IO.Path]::GetTempPath()) ('AshDesk-openh264-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $work | Out-Null
    try {
        $bz2 = Join-Path $work "$OpenH264Dll.bz2"
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $OpenH264Url -OutFile $bz2 -UseBasicParsing
        Assert-Sha256 $bz2 $OpenH264Bz2Sha256
        $dll = Join-Path $work $OpenH264Dll
        Expand-Bzip2 $bz2 $dll
        Assert-Sha256 $dll $OpenH264DllSha256
        Copy-Item $dll $Bin -Force
    } finally {
        Remove-Item $work -Recurse -Force
    }
}

function Assert-Sha256([string]$Path, [string]$Expected) {
    $actual = (Get-FileHash -Algorithm SHA256 $Path).Hash
    if ($actual -ne $Expected) { throw "$Path has sha256 $actual, expected $Expected" }
}

# Windows has no bzip2 decoder of its own: tar.exe only has one on some builds (Windows 11 yes; Windows 10 and
# Server 2022 no), .NET has none. This one follows toybox's bzcat (0BSD, Rob Landley): single stream, no randomised
# blocks; every block CRC and the stream CRC are checked, and the caller checks the sha256 of the result anyway.
$AshBunzipSource = @'
using System;
using System.IO;

public static class AshBunzip {
    sealed class Bits {
        readonly byte[] b; int pos; uint cur; int left;
        public Bits(byte[] b) { this.b = b; }
        public uint Get(int n) {
            uint v = 0;
            while (n > 0) {
                if (left == 0) {
                    if (pos >= b.Length) throw Bad("truncated");
                    cur = b[pos++]; left = 8;
                }
                int take = n < left ? n : left;
                v = (v << take) | ((cur >> (left - take)) & ((1u << take) - 1u));
                left -= take; n -= take;
            }
            return v;
        }
    }

    static InvalidDataException Bad(string why) { return new InvalidDataException("bzip2: " + why); }

    public static byte[] Decode(byte[] src) {
        Bits r = new Bits(src);
        if (r.Get(8) != 'B' || r.Get(8) != 'Z' || r.Get(8) != 'h') throw Bad("not a bzip2 stream");
        int level = (int)r.Get(8) - '0';
        if (level < 1 || level > 9) throw Bad("bad block size");
        int dbufSize = level * 100000;
        uint[] crcTab = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i << 24;
            for (int k = 0; k < 8; k++) c = (c & 0x80000000u) != 0 ? (c << 1) ^ 0x04C11DB7u : c << 1;
            crcTab[i] = c;
        }
        MemoryStream outp = new MemoryStream();
        int[] dbuf = new int[dbufSize];
        byte[] selectors = new byte[32768];
        uint total = 0;
        for (;;) {
            uint m1 = r.Get(24), m2 = r.Get(24), headerCrc = r.Get(32);
            if (m1 == 0x177245 && m2 == 0x385090) {
                if (headerCrc != total) throw Bad("stream CRC mismatch");
                break;
            }
            if (m1 != 0x314159 || m2 != 0x265359) throw Bad("bad block signature");
            if (r.Get(1) != 0) throw Bad("randomised blocks are not supported");
            int origPtr = (int)r.Get(24);

            byte[] symToByte = new byte[256];
            int symTotal = 0;
            uint used = r.Get(16);
            for (int i = 0; i < 16; i++) {
                if ((used & (0x8000u >> i)) == 0) continue;
                uint k = r.Get(16);
                for (int j = 0; j < 16; j++) if ((k & (0x8000u >> j)) != 0) symToByte[symTotal++] = (byte)(16 * i + j);
            }
            if (symTotal == 0) throw Bad("empty symbol map");

            int groups = (int)r.Get(3);
            if (groups < 2 || groups > 6) throw Bad("bad table count");
            int nSel = (int)r.Get(15);
            if (nSel == 0) throw Bad("no selectors");
            byte[] mtfG = { 0, 1, 2, 3, 4, 5 };
            for (int i = 0; i < nSel; i++) {
                int j = 0;
                while (r.Get(1) == 1) if (++j >= groups) throw Bad("bad selector");
                byte v = mtfG[j];
                for (int k = j; k > 0; k--) mtfG[k] = mtfG[k - 1];
                mtfG[0] = v;
                selectors[i] = v;
            }

            int symCount = symTotal + 2;
            int[][] limit = new int[groups][], bas = new int[groups][], perm = new int[groups][];
            int[] minLen = new int[groups], maxLen = new int[groups];
            for (int g = 0; g < groups; g++) {
                int[] len = new int[symCount];
                int h = (int)r.Get(5);
                for (int i = 0; i < symCount; i++) {
                    for (;;) {
                        if (h < 1 || h > 20) throw Bad("bad code length");
                        if (r.Get(1) == 0) break;
                        h += r.Get(1) == 0 ? 1 : -1;
                    }
                    len[i] = h;
                }
                int mn = 21, mx = 0;
                foreach (int l in len) { if (l < mn) mn = l; if (l > mx) mx = l; }
                int[] count = new int[22], lim = new int[22], bs = new int[22], pm = new int[symCount];
                foreach (int l in len) count[l]++;
                int p = 0;
                for (int l = mn; l <= mx; l++) for (int s = 0; s < symCount; s++) if (len[s] == l) pm[p++] = s;
                int code = 0, idx = 0;
                for (int l = mn; l <= mx; l++) {     /* canonical codes: the first code of each length follows the last one of the shorter length */
                    bs[l] = code - idx;
                    lim[l] = code + count[l] - 1;
                    idx += count[l];
                    code = (code + count[l]) << 1;
                }
                limit[g] = lim; bas[g] = bs; perm[g] = pm; minLen[g] = mn; maxLen[g] = mx;
            }

            byte[] mtf = new byte[256];
            for (int i = 0; i < 256; i++) mtf[i] = (byte)i;
            int[] counts = new int[256];
            int n = 0, sel = 0, groupLeft = 0, cg = 0, runPos = 0, run = 0, eob = symTotal + 1;
            for (;;) {
                if (groupLeft == 0) {
                    if (sel >= nSel) throw Bad("ran out of selectors");
                    cg = selectors[sel++];
                    groupLeft = 50;
                }
                groupLeft--;
                int l = minLen[cg];
                int v = (int)r.Get(l);
                while (v > limit[cg][l]) {
                    if (++l > maxLen[cg]) throw Bad("bad Huffman code");
                    v = (v << 1) | (int)r.Get(1);
                }
                int pi = v - bas[cg][l];
                if (pi < 0 || pi >= symCount) throw Bad("bad Huffman code");
                int sym = perm[cg][pi];
                if (sym <= 1) {                      /* RUNA / RUNB: bijective base-2 digits of a run of the front byte */
                    if (runPos == 0) { runPos = 1; run = 0; }
                    run += runPos << sym;
                    runPos <<= 1;
                    if (run > dbufSize) throw Bad("run too long");
                    continue;
                }
                if (runPos != 0) {
                    runPos = 0;
                    if (n + run > dbufSize) throw Bad("block too long");
                    byte b = symToByte[mtf[0]];
                    counts[b] += run;
                    while (run-- > 0) dbuf[n++] = b;
                }
                if (sym == eob) break;
                if (n >= dbufSize) throw Bad("block too long");
                int m = sym - 1;
                byte u = mtf[m];
                for (int t = m; t > 0; t--) mtf[t] = mtf[t - 1];
                mtf[0] = u;
                byte c = symToByte[u];
                counts[c]++;
                dbuf[n++] = c;
            }
            if (origPtr >= n) throw Bad("bad origin pointer");

            /* inverse BWT: low byte keeps the symbol, the upper bits get the index of the next one */
            int sum = 0;
            for (int i = 0; i < 256; i++) { int t = counts[i]; counts[i] = sum; sum += t; }
            for (int i = 0; i < n; i++) { int c = dbuf[i] & 0xff; dbuf[counts[c]] |= i << 8; counts[c]++; }

            /* undo the initial run-length step: after 4 equal bytes the next value is a repeat count */
            uint crc = 0xffffffffu;
            int wpos = dbuf[origPtr], current = wpos & 0xff, runc = -1;
            wpos >>= 8;
            for (int k = n; k > 0; k--) {
                int previous = current;
                wpos = dbuf[wpos];
                current = wpos & 0xff;
                wpos >>= 8;
                int copies, outb;
                if (runc++ == 3) { copies = current; outb = previous; current = -1; }
                else { copies = 1; outb = current; }
                while (copies-- > 0) {
                    outp.WriteByte((byte)outb);
                    crc = (crc << 8) ^ crcTab[(crc >> 24) ^ (uint)outb];
                }
                if (current != previous) runc = 0;
            }
            crc = ~crc;
            if (crc != headerCrc) throw Bad("block CRC mismatch");
            total = ((total << 1) | (total >> 31)) ^ crc;
        }
        return outp.ToArray();
    }
}
'@

function Expand-Bzip2([string]$Bz2, [string]$Out) {
    if (-not ('AshBunzip' -as [type])) { Add-Type -TypeDefinition $AshBunzipSource -Language CSharp }
    [IO.File]::WriteAllBytes($Out, [AshBunzip]::Decode([IO.File]::ReadAllBytes($Bz2)))
}
