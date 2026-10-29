const { analyse } = require('chardet');

const MAX_LYRIC_BYTES = 1000000;

function decodeKnown(bytes, encoding) {
    const name = encoding.toLowerCase();
    if (name === 'utf-32le' || name === 'utf-32be') {
        if (bytes.length % 4 !== 0) {
            throw new Error('UTF-32 歌词字节数无效');
        }
        const characters = [];
        for (let offset = 0; offset < bytes.length; offset += 4) {
            const point = name === 'utf-32le' ? bytes.readUInt32LE(offset) : bytes.readUInt32BE(offset);
            if (point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
                throw new Error('UTF-32 歌词包含无效字符');
            }
            characters.push(String.fromCodePoint(point));
        }
        return characters.join('').replace(/^\uFEFF/, '');
    }
    return new TextDecoder(name === 'ascii' ? 'utf-8' : name, { fatal: true }).decode(bytes);
}

/** 先识别明确的 Unicode 标记，再用原始字节检测旧编码；不猜测或改写已有 Unicode 文本。 */
function decodeLyrics(bytes) {
    if (!Buffer.isBuffer(bytes) || bytes.length > MAX_LYRIC_BYTES) {
        throw new Error('歌词内容无效或过大');
    }
    const prefix = bytes.subarray(0, 4).toString('hex');
    const markers = [
        ['fffe0000', 'utf-32le'],
        ['0000feff', 'utf-32be'],
        ['efbbbf', 'utf-8'],
        ['fffe', 'utf-16le'],
        ['feff', 'utf-16be'],
    ];
    for (const [marker, encoding] of markers) {
        if (prefix.startsWith(marker)) {
            return { text: decodeKnown(bytes, encoding), encoding };
        }
    }
    // 无 BOM 的 UTF-16 时间标签有稳定的交错零字节，不能先作为合法 UTF-8 接受。
    const zeros = [0, 0];
    for (let index = 0; index < Math.min(bytes.length, 128); index++) {
        if (bytes[index] === 0) {
            zeros[index % 2]++;
        }
    }
    for (const [parity, encoding] of [
        [1, 'utf-16le'],
        [0, 'utf-16be'],
    ]) {
        if (zeros[parity] >= 4 && zeros[parity] > zeros[1 - parity] * 4) {
            return { text: decodeKnown(bytes, encoding), encoding };
        }
    }
    // ISO-2022-JP 由 ASCII 转义序列切换字符集；这些字节本身也能通过 UTF-8 校验。
    if (/\x1b(?:\$[@B]|\([BJ])/.test(bytes.toString('latin1'))) {
        return { text: decodeKnown(bytes, 'iso-2022-jp'), encoding: 'iso-2022-jp' };
    }
    try {
        return { text: decodeKnown(bytes, 'utf-8'), encoding: 'utf-8' };
    } catch {
        // 严格 UTF-8 校验失败后才检测 Shift-JIS、EUC-JP 等旧编码，避免破坏正常歌词。
    }
    for (const candidate of analyse(bytes.subarray(0, 65536))) {
        if (candidate.confidence < 10) {
            continue;
        }
        try {
            return { text: decodeKnown(bytes, candidate.name), encoding: candidate.name.toLowerCase() };
        } catch {
            // 探测结果还必须能完整解码；不支持或含非法字节时继续尝试下一候选。
        }
    }
    throw new Error('无法识别歌词编码');
}

module.exports = { decodeLyrics, MAX_LYRIC_BYTES };
