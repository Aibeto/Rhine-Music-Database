// D06：Vulkan 资源策略的纯函数用例。
//
// 这里钉的都是"不会崩、而是悄悄画错"的边界：偏移没对齐、range 超过单次绑定上限、
// 同一个 SRB 的两个动态偏移被去重成一次、arena 换代后还沿用旧缓存、直写漏了槽位。
// 依赖：Qt6::Core；不创建 QGuiApplication，也不碰 VkDevice。
#include "render/VulkanResourcePolicy.h"
#include "render/VulkanResourceProbe.h"

#include <QString>
#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

bool check(bool result, const QString &what, const QString &expected, const QString &actual)
{
    ++g_checks;
    if (!result) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n       期望: %s\n       实际: %s\n", what.toUtf8().constData(),
                     expected.toUtf8().constData(), actual.toUtf8().constData());
    }
    return result;
}

QString number(quint64 value)
{
    return QString::number(value);
}

bool stridePacking()
{
    bool ok = true;
    ok &= check(RhineRender::arenaStride(128, 256) == 256, QStringLiteral("128 字节按 256 对齐"),
                QStringLiteral("256"), number(RhineRender::arenaStride(128, 256)));
    ok &= check(RhineRender::arenaStride(128, 64) == 128, QStringLiteral("128 字节按 64 对齐"),
                QStringLiteral("128"), number(RhineRender::arenaStride(128, 64)));
    ok &= check(RhineRender::arenaStride(1, 256) == 256, QStringLiteral("1 字节也要占满一个对齐步长"),
                QStringLiteral("256"), number(RhineRender::arenaStride(1, 256)));
    ok &= check(RhineRender::arenaStride(128, 0) == 0, QStringLiteral("对齐为 0 视为非法"),
                QStringLiteral("0"), number(RhineRender::arenaStride(128, 0)));
    ok &= check(RhineRender::arenaStride(128, 3) == 0, QStringLiteral("非 2 的幂对齐视为非法"),
                QStringLiteral("0"), number(RhineRender::arenaStride(128, 3)));
    ok &= check(RhineRender::arenaStride(0, 256) == 0, QStringLiteral("元素为 0 字节视为非法"),
                QStringLiteral("0"), number(RhineRender::arenaStride(0, 256)));
    ok &= check(RhineRender::alignTo(129, 4) == 132, QStringLiteral("129 对齐到 4"),
                QStringLiteral("132"), number(RhineRender::alignTo(129, 4)));
    ok &= check(RhineRender::alignTo(128, 4) == 128, QStringLiteral("已对齐的值不变"),
                QStringLiteral("128"), number(RhineRender::alignTo(128, 4)));
    return ok;
}

bool offsetRanges()
{
    bool ok = true;
    constexpr quint64 maxRange = 65536;
    constexpr quint64 capacity = 65536;
    ok &= check(RhineRender::dynamicOffsetFits(0, 128, capacity, maxRange),
                QStringLiteral("常规槽位应通过"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::dynamicOffsetFits(65536 - 64, 128, capacity, maxRange),
                QStringLiteral("越过 buffer 末尾必须拒绝"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::dynamicOffsetFits(capacity - 128, 128, capacity, maxRange),
                QStringLiteral("恰好贴住末尾应通过"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::dynamicOffsetFits(0, maxRange + 1, capacity, maxRange),
                QStringLiteral("单次 range 超过 MaxUniformBufferRange 必须拒绝"),
                QStringLiteral("false"), QStringLiteral("true"));
    ok &= check(!RhineRender::dynamicOffsetFits(100, 128, capacity, maxRange, 256),
                QStringLiteral("偏移没按对齐步长落位必须拒绝"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::dynamicOffsetFits(256, 128, capacity, maxRange, 256),
                QStringLiteral("对齐的槽位偏移应通过"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::dynamicOffsetFits(0xfffffffffffffff0ull, 128, 0xffffffffffffffffull,
                                               maxRange),
                QStringLiteral("offset+range 溢出必须拒绝"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(!RhineRender::dynamicOffsetFits(0, 0, capacity, maxRange),
                QStringLiteral("range 为 0 视为非法"), QStringLiteral("false"), QStringLiteral("true"));
    return ok;
}

bool capacityPlanning()
{
    bool ok = true;
    ok &= check(RhineRender::arenaCapacityFor(256, 64, 1ull << 20) == 16384,
                QStringLiteral("64 个 256 字节槽位"), QStringLiteral("16384"),
                number(RhineRender::arenaCapacityFor(256, 64, 1ull << 20)));
    ok &= check(RhineRender::arenaCapacityFor(256, 64, 1024) == 0,
                QStringLiteral("超过上限就返回 0 交给调用方回退"), QStringLiteral("0"),
                number(RhineRender::arenaCapacityFor(256, 64, 1024)));
    ok &= check(RhineRender::arenaCapacityFor(0, 64, 1ull << 20) == 0,
                QStringLiteral("步长为 0 不可规划"), QStringLiteral("0"),
                number(RhineRender::arenaCapacityFor(0, 64, 1ull << 20)));
    ok &= check(RhineRender::arenaCapacityFor(256, 0, 1ull << 20) == 0,
                QStringLiteral("0 个槽位没有意义"), QStringLiteral("0"),
                number(RhineRender::arenaCapacityFor(256, 0, 1ull << 20)));
    return ok;
}

bool instancePoolGrowth()
{
    bool ok = true;
    constexpr quint64 floorBytes = 4ull << 20;
    ok &= check(RhineRender::instancePoolBytes(1024, 0) == floorBytes,
                QStringLiteral("首次分配至少 4 MiB"), number(floorBytes),
                number(RhineRender::instancePoolBytes(1024, 0)));
    ok &= check(RhineRender::instancePoolBytes(1024, floorBytes) == floorBytes,
                QStringLiteral("够用就不重建"), number(floorBytes),
                number(RhineRender::instancePoolBytes(1024, floorBytes)));
    ok &= check(RhineRender::instancePoolBytes(floorBytes + 1, floorBytes) == floorBytes * 2,
                QStringLiteral("不够就翻倍"), number(floorBytes * 2),
                number(RhineRender::instancePoolBytes(floorBytes + 1, floorBytes)));
    constexpr quint64 maxBytes = 8ull << 20;
    ok &= check(RhineRender::instancePoolBytes(maxBytes + 1, 0, maxBytes) == 0,
                QStringLiteral("超过上限返回 0（调用方回落单 draw buffer）"), QStringLiteral("0"),
                number(RhineRender::instancePoolBytes(maxBytes + 1, 0, maxBytes)));
    ok &= check(RhineRender::instancePoolBytes(maxBytes, 0, maxBytes) == maxBytes,
                QStringLiteral("恰好等于上限可用"), number(maxBytes),
                number(RhineRender::instancePoolBytes(maxBytes, 0, maxBytes)));
    return ok;
}

bool poolWrites()
{
    bool ok = true;
    ok &= check(RhineRender::poolWriteNeeded(false, 256, 256),
                QStringLiteral("内容变了必须写"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::poolWriteNeeded(true, 256, 256),
                QStringLiteral("内容与落点都没变可以跳过"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::poolWriteNeeded(true, 512, 256),
                QStringLiteral("内容相同但偏移变了必须重写"), QStringLiteral("true"),
                QStringLiteral("false"));
    ok &= check(RhineRender::bindingRebindRequired(false, true, true, 256, 256),
                QStringLiteral("换了 SRB 必须重绑"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(RhineRender::bindingRebindRequired(true, true, true, 512, 256),
                QStringLiteral("同 SRB 不同动态偏移必须重绑"), QStringLiteral("true"),
                QStringLiteral("false"));
    ok &= check(!RhineRender::bindingRebindRequired(true, true, true, 256, 256),
                QStringLiteral("同 SRB 同偏移可以跳过"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::bindingRebindRequired(true, false, true, 0, 0),
                QStringLiteral("动态标志变化必须重绑"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::bindingRebindRequired(true, false, false, 512, 0),
                QStringLiteral("静态绑定的偏移不参与比较"), QStringLiteral("false"),
                QStringLiteral("true"));
    return ok;
}

bool sharedCacheLifetime()
{
    bool ok = true;
    ok &= check(RhineRender::resourceGenerationMatches(3, 3), QStringLiteral("同一代号可用"),
                QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::resourceGenerationMatches(2, 3),
                QStringLiteral("arena 换代后旧缓存不可用"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(!RhineRender::pruneRetireNeeded(true, true),
                QStringLiteral("本帧用过且代号一致的不退休"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::pruneRetireNeeded(false, true),
                QStringLiteral("本帧没用过的要退休"), QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(RhineRender::pruneRetireNeeded(true, false),
                QStringLiteral("代号过期的一律退休"), QStringLiteral("true"), QStringLiteral("false"));
    return ok;
}

bool depthConsumers()
{
    bool ok = true;
    // 真值表：任一消费者存在就必须 store，全都没有才允许不 store。
    for (int mask = 0; mask < 16; ++mask) {
        const bool motion = (mask & 1) != 0;
        const bool temporal = (mask & 2) != 0;
        const bool warp = (mask & 4) != 0;
        const bool metal = (mask & 8) != 0;
        const bool expected = mask != 0;
        const bool actual = RhineRender::depthStoreRequired(motion, temporal, warp, metal);
        ok &= check(actual == expected, QStringLiteral("深度消费者真值表 mask=%1").arg(mask),
                    expected ? QStringLiteral("store") : QStringLiteral("discard"),
                    actual ? QStringLiteral("store") : QStringLiteral("discard"));
    }
    return ok;
}

bool directWriteCoverage()
{
    bool ok = true;
    ok &= check(RhineRender::directWriteCoversAllSlots(4, 4), QStringLiteral("写满所有槽位"),
                QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::directWriteCoversAllSlots(3, 4),
                QStringLiteral("漏掉槽位会把上一帧残值读出来"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::directWriteCoversAllSlots(0, 0), QStringLiteral("没有槽位时平凡成立"),
                QStringLiteral("true"), QStringLiteral("false"));
    return ok;
}

bool materialSlotPlanning()
{
    bool ok = true;
    // 固定 40 个活动键、每轮整批换人：上一轮槽位全部可回收，水位不应上涨。
    quint32 highWater = 40;
    bool bounded = true;
    for (int round = 0; round < 1000; ++round) {
        const auto plan = RhineRender::planMaterialSlots(40, 0, highWater, highWater);
        bounded &= plan.fresh == 0 && plan.needSlots == 40;
        highWater = plan.needSlots;
    }
    ok &= check(bounded, QStringLiteral("1000 轮固定活动集合的 churn 不得抬高槽位水位"),
                QStringLiteral("needSlots 恒 40"), number(highWater));
    ok &= check(RhineRender::materialSlotPlanFits(40, 256),
                QStringLiteral("40 槽在 256 上限内"), QStringLiteral("true"), QStringLiteral("false"));

    // 半新半旧：30 个仍在映射，10 个空闲槽正好接住 10 个新键，不需要扩容。
    const auto reuse = RhineRender::planMaterialSlots(40, 30, 10, 40);
    ok &= check(reuse.reused == 10 && reuse.fresh == 0 && reuse.needSlots == 40,
                QStringLiteral("空闲槽优先复用且不计入扩容"), QStringLiteral("reused=10 fresh=0 need=40"),
                QStringLiteral("reused=%1 fresh=%2 need=%3")
                    .arg(reuse.reused)
                    .arg(reuse.fresh)
                    .arg(reuse.needSlots));

    // 全新 40 个键、没有任何空闲槽：只能把水位抬到 80。
    const auto grow = RhineRender::planMaterialSlots(40, 0, 0, 40);
    ok &= check(grow.fresh == 40 && grow.needSlots == 80,
                QStringLiteral("没有空闲槽时才扩容"), QStringLiteral("fresh=40 need=80"),
                QStringLiteral("fresh=%1 need=%2").arg(grow.fresh).arg(grow.needSlots));

    // 超出上限的真实活动集合：必须判定为不适合 arena，而不是悄悄越界。
    const auto over = RhineRender::planMaterialSlots(300, 0, 0, 0);
    ok &= check(!RhineRender::materialSlotPlanFits(over.needSlots, 256),
                QStringLiteral("超过 256 槽的活动集合必须回退"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(over.needSlots == 300, QStringLiteral("回退前先把真实需求算出来"),
                QStringLiteral("300"), number(over.needSlots));
    return ok;
}

bool directWriteAndPoolPlanning()
{
    bool ok = true;
    // 直写：内容与上一帧相同也必须写（否则本帧原生副本里留的是别的帧的槽位值）。
    ok &= check(RhineRender::materialWriteRequired(true, true),
                QStringLiteral("直写时内容相同也必须写"), QStringLiteral("true"),
                QStringLiteral("false"));
    ok &= check(!RhineRender::materialWriteRequired(false, true),
                QStringLiteral("batch 路径内容相同可以跳过"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(RhineRender::materialWriteRequired(false, false),
                QStringLiteral("batch 路径内容变了必须写"), QStringLiteral("true"),
                QStringLiteral("false"));

    // 两段列表统一规划：4 字节对齐，累计游标。
    quint64 cursor = 0;
    cursor = RhineRender::poolSliceEnd(cursor, 100);
    ok &= check(cursor == 100, QStringLiteral("首段按对齐落到 100"), QStringLiteral("100"),
                number(cursor));
    cursor = RhineRender::poolSliceEnd(cursor, 101);
    ok &= check(cursor == 201, QStringLiteral("第二段起点已对齐，末尾不额外 padding"),
                QStringLiteral("201"), number(cursor));
    cursor = RhineRender::poolSliceEnd(cursor, 0);
    ok &= check(cursor == 204, QStringLiteral("空切片只把游标推到下一个对齐点"), QStringLiteral("204"),
                number(cursor));

    // 只有透明列表时也要按同一套公式规划：起点对齐，长度原样累计。
    const quint64 transparentOnly = RhineRender::poolSliceEnd(0, 7);
    ok &= check(transparentOnly == 7, QStringLiteral("只有透明列表时 7 字节不补 padding"),
                QStringLiteral("7"), number(transparentOnly));
    ok &= check(RhineRender::poolSliceEnd(7, 4) == 12,
                QStringLiteral("起点要按 4 字节对齐（7→8）再加长度"), QStringLiteral("12"),
                number(RhineRender::poolSliceEnd(7, 4)));

    // 切片必须落在容量内；越界要拒绝而不是截断。
    ok &= check(RhineRender::poolSliceFits(200, 4, 204), QStringLiteral("贴住末尾可以"),
                QStringLiteral("true"), QStringLiteral("false"));
    ok &= check(!RhineRender::poolSliceFits(201, 4, 204),
                QStringLiteral("越过末尾必须拒绝"), QStringLiteral("false"), QStringLiteral("true"));
    ok &= check(!RhineRender::poolSliceFits(0, 205, 204),
                QStringLiteral("单切片超过容量必须拒绝"), QStringLiteral("false"),
                QStringLiteral("true"));
    ok &= check(!RhineRender::poolSliceFits(0xfffffffffffffff0ull, 0x40, 0xffffffffffffffffull),
                QStringLiteral("offset+bytes 溢出必须拒绝"), QStringLiteral("false"),
                QStringLiteral("true"));
    return ok;
}

bool configLine()
{
    bool ok = true;
    RhineRender::VulkanResourceConfig off;
    const auto offLine = RhineRender::formatVulkanConfigLine(off);
    ok &= check(offLine.contains(QStringLiteral("arena 关")), QStringLiteral("全关时 arena 显示关"),
                QStringLiteral("arena 关"), offLine);
    ok &= check(offLine.contains(QStringLiteral("深度不保留 关")),
                QStringLiteral("全关时深度不保留显示关"), QStringLiteral("深度不保留 关"), offLine);

    RhineRender::VulkanResourceConfig on;
    on.arenaRequested = true;
    on.arenaEffective = true;
    on.discardDepthRequested = true;
    on.discardDepthEffective = true;
    const auto onLine = RhineRender::formatVulkanConfigLine(on);
    ok &= check(onLine.contains(QStringLiteral("arena 开")), QStringLiteral("生效时显示开"),
                QStringLiteral("arena 开"), onLine);
    ok &= check(onLine.contains(QStringLiteral("（回退 -）")), QStringLiteral("没回退时占位为 -"),
                QStringLiteral("（回退 -）"), onLine);

    RhineRender::VulkanResourceConfig fallback;
    fallback.arenaRequested = true;
    fallback.arenaEffective = false;
    fallback.arenaFallback = QStringLiteral("材质槽位超过 256");
    const auto fallbackLine = RhineRender::formatVulkanConfigLine(fallback);
    ok &= check(fallbackLine.contains(QStringLiteral("请求开→已回退")),
                QStringLiteral("请求开但生效关必须显式标出回退"), QStringLiteral("请求开→已回退"),
                fallbackLine);
    ok &= check(fallbackLine.contains(QStringLiteral("材质槽位超过 256")),
                QStringLiteral("回退原因必须打印"), QStringLiteral("材质槽位超过 256"), fallbackLine);
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= stridePacking();
    ok &= offsetRanges();
    ok &= capacityPlanning();
    ok &= instancePoolGrowth();
    ok &= poolWrites();
    ok &= sharedCacheLifetime();
    ok &= depthConsumers();
    ok &= directWriteCoverage();
    ok &= materialSlotPlanning();
    ok &= directWriteAndPoolPlanning();
    ok &= configLine();
    std::printf("Vulkan 资源策略: %d 项检查，%d 项失败\n", g_checks, g_failures);
    return ok && g_failures == 0 ? 0 : 1;
}
