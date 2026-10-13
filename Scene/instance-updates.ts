import { InstancedBufferAttribute } from 'three';

/** 比较实际上传的 Float32 值，只上传连续的变更范围。 */
export class InstanceUpdates {
    private first = Infinity;
    private last = -1;
    readonly attribute: InstancedBufferAttribute;
    constructor(attribute: InstancedBufferAttribute) {
        this.attribute = attribute;
    }

    set(offset: number, values: ArrayLike<number>) {
        const array = this.attribute.array;
        for (let j = 0; j < values.length; j++) {
            const i = offset + j;
            const value = Math.fround(values[j]);
            if (array[i] === value) {
                continue;
            }
            array[i] = value;
            this.first = Math.min(this.first, i);
            this.last = Math.max(this.last, i);
        }
    }

    scalar(offset: number, value: number) {
        value = Math.fround(value);
        if (this.attribute.array[offset] === value) {
            return;
        }
        this.attribute.array[offset] = value;
        this.first = Math.min(this.first, offset);
        this.last = Math.max(this.last, offset);
    }

    commit() {
        if (this.last < 0) {
            return false;
        }
        // 尚未发生渲染时，不丢弃等待上传的变更范围。
        this.attribute.addUpdateRange(this.first, this.last - this.first + 1);
        this.attribute.needsUpdate = true;
        this.first = Infinity;
        this.last = -1;
        return true;
    }
}
