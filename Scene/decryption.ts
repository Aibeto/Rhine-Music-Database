// 按本地参考的 25 帧/秒测量；这里使用原视频的秒数，
// 与相机沿用的“原时间减五秒”约定不同。
export const DECRYPTION_START = 34.12;
export const DECRYPTION_END = 39.56;
const INTERACTIVE_RATE = 1.5;
const clamp = (value: number) => Math.max(0, Math.min(1, value));
const smooth = (value: number) => {
    const x = clamp(value);
    return x * x * x * (10 + x * (-15 + x * 6));
};
type Knot = readonly [number, number];
// 单调三次 Hermite 插值保留测得的缓动，
// 避免采样帧处过冲或速度突变，端点速度为零。
export function sampleCurve(knots: readonly Knot[], time: number) {
    if (time <= knots[0][0]) {
        return knots[0][1];
    }
    if (time >= knots[knots.length - 1][0]) {
        return knots[knots.length - 1][1];
    }
    const secant = (i: number) => (knots[i + 1][1] - knots[i][1]) / (knots[i + 1][0] - knots[i][0]);
    const slope = (i: number) => {
        if (i === 0 || i === knots.length - 1) {
            return 0;
        }
        const a = secant(i - 1);
        const b = secant(i);
        if (a * b <= 0) {
            return 0;
        }
        const left = knots[i][0] - knots[i - 1][0];
        const right = knots[i + 1][0] - knots[i][0];
        const w1 = 2 * right + left;
        const w2 = right + 2 * left;
        return (w1 + w2) / (w1 / a + w2 / b);
    };
    let i = 0;
    while (time > knots[i + 1][0]) {
        i++;
    }
    const span = knots[i + 1][0] - knots[i][0];
    const t = (time - knots[i][0]) / span;
    const t2 = t * t;
    const t3 = t2 * t;
    return (
        (2 * t3 - 3 * t2 + 1) * knots[i][1] +
        (t3 - 2 * t2 + t) * span * slope(i) +
        (-2 * t3 + 3 * t2) * knots[i + 1][1] +
        (t3 - t2) * span * slope(i + 1)
    );
}
const GROW: readonly Knot[] = [
    [34.24, 0],
    [34.4, 0.19],
    [34.64, 0.57],
    [34.96, 0.79],
    [35.28, 0.92],
    [35.6, 0.973],
    [36.04, 1],
];
// 透明前沿从线条回缩结束时开始；第 39 秒时上半部已可读，
// 随高度变化的粗糙度与前沿同步移动。
const REVEAL: readonly Knot[] = [
    [38.84, 0],
    [38.92, 0.28],
    [39.0, 0.51],
    [39.16, 0.74],
    [39.32, 0.94],
    [39.56, 1],
];
const RETRACT: readonly Knot[] = [
    [37.72, 1],
    [37.88, 0.72],
    [38.0, 0.38],
    [38.12, 0.22],
    [38.24, 0.14],
    [38.4, 0.075],
    [38.64, 0.024],
    [38.84, 0],
];

// 端点在第 37 秒的表面上测量；所有移动端点沿模型空间的
// 同一条对角线运动，投影随相机变化。
export const SCAN_FROM = [-1.6, 0.5] as const;
export const SCAN_TO = [1.98, 3.24] as const;
export const SCAN_CORNERS = [
    [-1.88, 3.2],
    [2.14, 3.36],
    [-1.77, 0.36],
    [2.17, 0.65],
] as const;
export function decryptionFrame(time: number) {
    const grow = sampleCurve(GROW, time);
    const remaining = sampleCurve(RETRACT, time);
    const intervals: [number, number][] = [];
    if (time >= 34.24 && time < 36.04 && grow > 0) {
        intervals.push([0, grow * 0.5], [1 - grow * 0.5, 1]);
    } else if (time >= 36.04 && time < 38.84) {
        intervals.push([0.5 - remaining * 0.5, 0.5 + remaining * 0.5]);
    }
    const markers = smooth((time - 34.2) / 0.12) * (1 - smooth((time - 37.76) / 0.56));
    // 保持原有判断顺序，仅计算当前需要的分支结果。
    // 按原视频时间节点选择解密阶段。
    const selectDecryptionPhase = () => {
        if (time < 34.24) {
            return 'waiting';
        } else if (time < 36.04) {
            return 'joining';
        } else if (time < 37.72) {
            return 'connected';
        } else if (time < 38.84) {
            return 'retracting';
        } else if (time < DECRYPTION_END) {
            return 'revealing';
        } else {
            return 'clear';
        }
    };
    return {
        time,
        intervals,
        markers,
        point: smooth((time - 38.58) / 0.2) * (1 - smooth((time - 39.08) / 0.22)),
        label: smooth((time - 34.32) / 0.36) * (1 - smooth((time - 37.68) / 0.24)),
        labelValue: smooth((time - 35.64) / 0.56),
        clarity: sampleCurve(REVEAL, time),
        phase: selectDecryptionPhase(),
    };
}
export type DecryptionFrame = ReturnType<typeof decryptionFrame>;
export class DecryptionController {
    clarity = 0;
    private active = false;
    private elapsed: number | null = null;
    frame: DecryptionFrame = decryptionFrame(-1);
    enter(alreadyClear = false) {
        if (this.active) {
            return;
        }
        this.active = true;
        this.elapsed = alreadyClear ? (DECRYPTION_END - DECRYPTION_START) / INTERACTIVE_RATE : null;
        if (alreadyClear) {
            this.finish();
        }
    }

    leave() {
        this.active = false;
        this.elapsed = null;
        this.frame = decryptionFrame(-1);
    }

    select(clarity = 0) {
        this.leave();
        this.clarity = clarity;
    }

    finish() {
        this.elapsed = (DECRYPTION_END - DECRYPTION_START) / INTERACTIVE_RATE;
        this.clarity = 1;
        this.frame = decryptionFrame(DECRYPTION_END);
    }

    update(dt: number, ready: boolean, reduced: boolean, referenceTime?: number) {
        if (referenceTime !== undefined) {
            this.frame = decryptionFrame(referenceTime);
            this.clarity = this.frame.clarity;
            return;
        }
        if (!this.active) {
            this.clarity = reduced ? 0 : this.clarity * Math.exp(-Math.max(0, dt) * 9);
            if (this.clarity < 0.0001) {
                this.clarity = 0;
            }
            this.frame = decryptionFrame(-1);
            return;
        }
        if (reduced) {
            this.finish();
            return;
        }
        if (this.elapsed === null && ready) {
            this.elapsed = 0;
        } else if (this.elapsed !== null) {
            this.elapsed = Math.min(
                this.elapsed + Math.max(0, dt),
                (DECRYPTION_END - DECRYPTION_START) / INTERACTIVE_RATE,
            );
        }
        if (this.elapsed !== null) {
            this.frame = decryptionFrame(DECRYPTION_START + this.elapsed * INTERACTIVE_RATE);
            // 重新结霜时再次进入，从当前显示的材质状态继续。
            if (this.frame.clarity > this.clarity) {
                this.clarity = this.frame.clarity;
            } else if (this.frame.phase === 'clear') {
                this.clarity = 1;
            } else {
                this.clarity = this.clarity * Math.exp(-Math.max(0, dt) * 9);
            }
        }
    }
}
