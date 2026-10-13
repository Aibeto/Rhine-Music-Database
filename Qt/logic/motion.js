// 沿用旧版滚字的 49 个弹簧采样点和弹窗贝塞尔曲线，连续打断接续当前速度。
export class ReelCurve {
    constructor(from, target, velocity, start, duration = 0.46) {
        this.start = start;
        this.duration = duration;
        const s = from - target;
        const cap = (Math.max(Math.abs(s), 1) * 12) / duration;
        const h = Math.max(-cap, Math.min(cap, velocity)) * duration;
        this.points = [];
        for (let i = 0; i <= 48; i++) {
            const t = i / 48;
            this.points.push(i === 48 ? target : target + (s + (h + 10 * s) * t) * Math.exp(-10 * t));
        }
    }

    sample(time) {
        const p = Math.max(0, Math.min(1, (time - this.start) / this.duration)) * 48;
        if (p >= 48) {
            return [this.points[48], 0];
        }
        const i = Math.floor(p);
        return [
            this.points[i] + (this.points[i + 1] - this.points[i]) * (p - i),
            ((this.points[i + 1] - this.points[i]) * 48) / this.duration,
        ];
    }
}

function bezier(x, x1, y1, x2, y2) {
    const component = (t, a, b) => 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t;
    let lo = 0;
    let hi = 1;
    for (let i = 0; i < 24; i++) {
        const t = (lo + hi) / 2;
        if (component(t, x1, x2) < x) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return component((lo + hi) / 2, y1, y2);
}

export class SurfaceMotion {
    constructor() {
        this.opacity = 0;
        this.offset = 12;
        this.duration = 0;
        this.fromOpacity = 0;
        this.fromOffset = 12;
        this.targetOpacity = 0;
        this.targetOffset = 12;
        this.start = 0;
        this.entering = true;
    }

    get finished() {
        return this.duration === 0;
    }

    sample(time) {
        if (this.duration <= 0) {
            return;
        }
        const p = Math.min(1, Math.max(0, (time - this.start) / this.duration));
        const e = bezier(p, this.entering ? 0.22 : 0.4, this.entering ? 1 : 0, this.entering ? 0.36 : 1, 1);
        this.opacity = this.fromOpacity + (this.targetOpacity - this.fromOpacity) * e;
        this.offset = this.fromOffset + (this.targetOffset - this.fromOffset) * e;
        if (p >= 1) {
            this.duration = 0;
        }
    }

    set(shown, time, reduced = false) {
        this.sample(time);
        this.fromOpacity = this.opacity;
        this.fromOffset = this.offset;
        this.targetOpacity = shown ? 1 : 0;
        this.targetOffset = shown ? 0 : 8;
        this.entering = shown;
        this.start = time;
        if (reduced) {
            this.duration = 0;
        } else if (shown) {
            this.duration = 0.3;
        } else {
            this.duration = 0.2;
        }
        if (reduced) {
            this.opacity = this.targetOpacity;
            this.offset = this.targetOffset;
        }
    }
}
const mod = (n, d) => ((n % d) + d) % d;
export class RollingLabel {
    constructor() {
        this.value = '';
        this.slots = [];
        this.retired = [];
        this.initialized = false;
    }

    active(time) {
        return this.slots
            .concat(this.retired)
            .some((slot) =>
                ['position', 'x', 'width', 'alpha'].some(
                    (key) => time < slot[key].start + slot[key].duration,
                ),
            );
    }

    render(hud, next, x, y, size, weight, tracking, time, animated, numeric) {
        const chars = Array.from(next);
        const oldNumber = parseInt(this.value) || 0;
        const newNumber = parseInt(next) || 0;
        if (next !== this.value || !this.initialized) {
            const previous = this.slots;
            const fresh = [];
            let cursor = 0;
            chars.forEach((ch, i) => {
                const width = hud.g.width(ch, size, weight, tracking) + tracking;
                const endX = cursor;
                cursor += width;
                if (i < previous.length && this.initialized && animated) {
                    const slot = previous[i];
                    const [p, v] = slot.position.sample(time);
                    const xx = slot.x.sample(time);
                    const ww = slot.width.sample(time);
                    if (
                        slot.wheel[mod(Math.floor(p), slot.wheel.length)] !== ch ||
                        Math.abs(p - Math.round(p)) > 0.00001
                    ) {
                        if (numeric && /^\d$/.test(ch)) {
                            let target = Math.floor(p / 10) * 10 + Number(ch);
                            if (newNumber > oldNumber && target < p) {
                                target += 10;
                            }
                            if (newNumber < oldNumber && target > p) {
                                target -= 10;
                            }
                            while (target < 0) {
                                target += 10;
                            }
                            slot.wheel = Array.from(
                                {
                                    length: 10,
                                },
                                (_, i) => String(i),
                            );
                            slot.position = new ReelCurve(p < 0 ? p + 10 : p, target, v, time);
                        } else {
                            const base = Math.floor(p);
                            const fraction = p - base;
                            const wheel = [slot.wheel[mod(base, slot.wheel.length)]];
                            if (fraction > 0.00001) {
                                wheel.push(slot.wheel[mod(base + 1, slot.wheel.length)]);
                            }
                            if (wheel[wheel.length - 1] !== ch) {
                                wheel.push(ch);
                            }
                            const target = wheel.length - 1;
                            slot.wheel = wheel;
                            slot.position = new ReelCurve(
                                fraction,
                                target,
                                Math.min(Math.max(0, v), ((target - fraction) * 10000) / 460),
                                time,
                            );
                        }
                    }
                    slot.x = new ReelCurve(xx[0], endX, xx[1], time);
                    slot.width = new ReelCurve(ww[0], width, ww[1], time);
                    slot.alpha = new ReelCurve(slot.alpha.sample(time)[0], 1, 0, time);
                    fresh.push(slot);
                } else {
                    const digit = numeric ? Number(ch) || 0 : 0;
                    fresh.push({
                        wheel:
                            numeric && /^\d$/.test(ch)
                                ? Array.from(
                                      {
                                          length: 10,
                                      },
                                      (_, i) => String(i),
                                  )
                                : [ch],
                        position: new ReelCurve(digit, digit, 0, time),
                        x: new ReelCurve(endX, endX, 0, time),
                        width: new ReelCurve(width, width, 0, time),
                        alpha: new ReelCurve(this.initialized && animated ? 0 : 1, 1, 0, time),
                    });
                }
            });
            this.retired = [];
            if (animated && previous.length > chars.length) {
                for (const slot of previous.slice(chars.length)) {
                    slot.alpha = new ReelCurve(slot.alpha.sample(time)[0], 0, 0, time, 0.46 * 0.65);
                    this.retired.push(slot);
                }
            }
            this.slots = fresh;
            this.value = next;
            this.initialized = true;
        }
        if (!animated || !this.active(time)) {
            this.retired = [];
            hud.text(next, x, y, size, weight, tracking);
            return;
        }
        const line = size * (numeric ? 1.1 : 1.326);
        for (const slot of this.slots.concat(this.retired)) {
            const p = slot.position.sample(time)[0];
            const xx = slot.x.sample(time)[0];
            const width = slot.width.sample(time)[0];
            const alpha = slot.alpha.sample(time)[0];
            if (alpha <= 0.001) {
                continue;
            }
            hud.save();
            hud.alpha(alpha);
            hud.clip(x + xx - size * 0.15, y, width + size * 0.3, line);
            const low = Math.floor(p);
            const high = Math.ceil(p);
            for (let index = low; index <= Math.max(low, high); index++) {
                hud.text(
                    slot.wheel[mod(index, slot.wheel.length)],
                    x + xx,
                    y + (index - p) * line,
                    size,
                    weight,
                    tracking,
                );
            }
            hud.restore();
        }
    }
}
