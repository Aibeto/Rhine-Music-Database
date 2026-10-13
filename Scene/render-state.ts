/** 精确比较状态，不改变动画状态，也不强制吸附。 */
export class RenderState {
    private values: (number | string | undefined)[] = [];
    private cursor = 0;
    private changed = true;
    begin() {
        this.cursor = 0;
    }

    add(...values: (number | string | undefined)[]) {
        for (const value of values) {
            if (this.values[this.cursor] !== value) {
                this.changed = true;
            }
            this.values[this.cursor++] = value;
        }
    }

    floats(...values: (number | undefined)[]) {
        // 与 GPU 浮点 uniform 对齐；标识和单调递增版本通过
        // add() 保存，避免长期运行丢失计数精度。
        this.add(...values.map((value) => (value === undefined ? undefined : Math.fround(value))));
    }

    end() {
        const changed = this.changed || this.values.length !== this.cursor;
        this.values.length = this.cursor;
        this.changed = false;
        return changed;
    }

    invalidate() {
        this.changed = true;
    }
}
