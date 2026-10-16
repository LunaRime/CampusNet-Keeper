// 深澜 xEncode：两个独立参考实现交叉验证 + 导出 C++ 测试向量
//   A = 门户自身 JS 的写法（无 padding，权威）
//   B = srunc (Rust, MIT) 的 quirk_base64 算法（有 padding）
// 二者仅在 padding 上不同；去 padding 后必须逐字节一致。
const TABLE = 'LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA';
if (new Set(TABLE).size !== 64) throw new Error('alphabet is not a permutation!');

function s(str, include_length) {
  const v = [];
  for (let i = 0; i < str.length; i += 4) {
    let word = 0;
    for (let j = 0; j < 4 && i + j < str.length; j++) word |= str.charCodeAt(i + j) << (8 * j);
    v.push(word >>> 0);
  }
  if (include_length) v.push(str.length);
  return v;
}
function l(v, k) {
  const n = v.length - 1;
  if (n < 1) return v;
  let z = v[n], y = v[0], d = 0;
  const q = Math.floor(6 + 52 / (n + 1));
  for (let i = 0; i < q; i++) {
    d = (d + 0x9E3779B9) >>> 0;
    const e = (d >>> 2) & 3;
    for (let p = 0; p < n; p++) {
      y = v[p + 1];
      let m = ((z >>> 5) ^ (y << 2)) >>> 0;
      m = (m + ((((y >>> 3) ^ (z << 4)) >>> 0) ^ (d ^ y))) >>> 0;
      m = (m + (((k[(p & 3) ^ e] || 0) ^ z) >>> 0)) >>> 0;
      z = v[p] = (v[p] + m) >>> 0;
    }
    y = v[0];
    let m = ((z >>> 5) ^ (y << 2)) >>> 0;
    m = (m + ((((y >>> 3) ^ (z << 4)) >>> 0) ^ (d ^ y))) >>> 0;
    m = (m + (((k[(n & 3) ^ e] || 0) ^ z) >>> 0)) >>> 0;
    z = v[n] = (v[n] + m) >>> 0;
  }
  return v;
}
function unbytes(v) {
  let out = '';
  for (const w of v) for (let j = 0; j < 4; j++) out += String.fromCharCode((w >>> (8 * j)) & 0xFF);
  return out;
}
// A：门户 JS 写法（6 位分组直接查表，无 padding）
function b64a(str) {
  if (!str) return '';
  let out = '', val = 0, bits = -6;
  for (let i = 0; i < str.length; i++) {
    val = ((val << 8) + str.charCodeAt(i)) >>> 0;
    bits += 8;
    while (bits >= 0) { out += TABLE.charAt((val >>> bits) & 0x3F); bits -= 6; }
  }
  if (bits > -6) out += TABLE.charAt(((val << 8) >>> (bits + 8)) & 0x3F);
  return out;
}
// B：srunc 写法（3 字节一组，高字节在前，带 '=' padding）
function b64b(t) {
  const bytes = [...t].map((c) => c.charCodeAt(0));
  const a = bytes.length;
  let out = '';
  for (let o = 0; o < a; o += 3) {
    const p2 = bytes[o];
    const p1 = o + 1 < a ? bytes[o + 1] : 0;
    const p0 = o + 2 < a ? bytes[o + 2] : 0;
    const h = (p2 << 16) | (p1 << 8) | p0;
    for (let i = 0; i < 4; i++) {
      if (o * 8 + i * 6 > a * 8) out += '=';
      else out += TABLE.charAt((h >>> (6 * (3 - i))) & 0x3F);
    }
  }
  return out;
}
const encodeA = (d, k) => (d === '' ? '' : b64a(unbytes(l(s(d, true), s(k, false)))));
const encodeB = (d, k) => (d === '' ? '' : b64b(unbytes(l(s(d, true), s(k, false)))));
// UTF-8 字节语义：先把字符串转成 UTF-8 字节串（latin1 表示）再打包。
// 门户自己的 JS 用 charCodeAt（非 ASCII 时是 UTF-16 码元），而 C++/Rust/Python 客户端
// 一律喂 UTF-8 字节；对纯 ASCII 两者等价，非 ASCII 才分叉（见 README 已知限制）。
const encodeUtf8 = (d, k) =>
  d === '' ? '' : b64a(unbytes(l(s(Buffer.from(d, 'utf8').toString('latin1'), true), s(k, false))));

console.log('base64A("abc") =', b64a('abc'), ' base64B("abc") =', b64b('abc'));

// 交叉验证：随机 + 边界长度 + 非 ASCII（用 UTF-8 字节语义，保证两边同口径）
let mism = 0, n = 0;
const keys = ['', 'k', 'key', '1234567890', 'srun-token-0001', '中文key'];
for (let len = 0; len <= 24; len++) {
  for (const key of keys) {
    let data = '';
    for (let i = 0; i < len; i++) data += String.fromCharCode((i * 37 + 11) % 256);
    const kb = Buffer.from(key, 'utf8').toString('latin1');
    n++;
    const a = encodeA(data, kb);
    const b = encodeB(data, kb).replace(/=+$/, '');
    if (a !== b) { mism++; if (mism < 4) console.log('MISMATCH len=' + len + ' key=' + JSON.stringify(key) + '\n  A=' + a + '\n  B=' + b); }
  }
}
console.log(`cross-check: ${n} cases, ${mism} mismatches (去 padding 后比较，UTF-8 字节语义)`);

console.log('\n--- 供 C++ 测试使用的向量（实现 A，UTF-8 字节语义）---');
const cases = [
  ['abc', 'key'],
  ['1', 'k'],
  ['hello', '1234567890'],
  ['{"acid":"1","enc_ver":"srun_bx1","ip":"10.0.0.5","password":"pw","username":"alice@telecom"}', 'simchallenge1'],
  ['中文密码@#', 'token'],
];
for (const [d, k] of cases) console.log(`"${d}" + "${k}" => ${encodeUtf8(d, k)}`);
