// CommandBus 单测:解析/分发/选项命令(increase/cycle/toggle)/错误拒绝。
#include <gtest/gtest.h>
#include "api/command_bus.h"
#include "options_generated.h"

namespace {
struct Fixture {
  rd::Options options;
  rd::CommandBus bus;
  Fixture() {
    rd::Options* o = &options;
    bus.add("set", [o](const std::string& a, std::string& out) {
      const auto sp = a.find(' ');
      if (sp == std::string::npos) return false;
      return rd::optionsSet(*o, a.substr(0, sp), a.substr(sp + 1));
    });
    bus.add("get", [o](const std::string& a, std::string& out) {
      return rd::optionsGet(*o, a, out);
    });
    bus.add("toggle", [o](const std::string& a, std::string& out) {
      std::string cur;
      if (!rd::optionsGet(*o, a, cur)) return false;
      if (cur != "true" && cur != "false") return false;
      return rd::optionsSet(*o, a, cur == "true" ? "false" : "true");
    });
  }
};
} // namespace

TEST(CommandBus, SetGet) {
  Fixture f;
  std::string out;
  EXPECT_TRUE(f.bus.exec("set render.exposure 2.0", out));
  EXPECT_TRUE(f.bus.exec("get render.exposure", out));
  EXPECT_EQ(out, "2.000000");
}

TEST(CommandBus, ToggleBool) {
  Fixture f;
  std::string out;
  EXPECT_FALSE(f.options.quality.fxaa);
  EXPECT_TRUE(f.bus.exec("toggle quality.fxaa", out));
  EXPECT_TRUE(f.options.quality.fxaa);
  EXPECT_TRUE(f.bus.exec("toggle quality.fxaa", out));
  EXPECT_FALSE(f.options.quality.fxaa);
  // 非 bool 拒绝
  EXPECT_FALSE(f.bus.exec("toggle render.exposure", out));
}

TEST(CommandBus, UnknownAndBadArgs) {
  Fixture f;
  std::string out;
  EXPECT_FALSE(f.bus.exec("nonsense", out));
  EXPECT_FALSE(f.bus.exec("set", out));           // 缺参数
  EXPECT_FALSE(f.bus.exec("set no.such 1", out));  // 未知名
}

TEST(CommandBus, CycleEnum) {
  Fixture f;
  std::string out;
  // 注册 cycle(enum 域循环)
  rd::Options* o = &f.options;
  f.bus.add("cycle", [o](const std::string& a, std::string& out) {
    const std::string dom = rd::optionsDomainJson(a);
    if (dom.empty()) return false;
    const auto vp = dom.find("\"values\"");
    if (vp == std::string::npos) return false;
    const auto lb = dom.find('[', vp), rb = dom.find(']', lb);
    std::string cur;
    if (!rd::optionsGet(*o, a, cur)) return false;
    std::vector<std::string> vals;
    for (size_t p = lb; (p = dom.find('"', p + 1)) != std::string::npos && p < rb;) {
      const size_t q = dom.find('"', p + 1);
      vals.push_back(dom.substr(p + 1, q - p - 1));
      p = q;
    }
    for (size_t i = 0; i < vals.size(); ++i)
      if (vals[i] == cur) return rd::optionsSet(*o, a, vals[(i + 1) % vals.size()]);
    return false;
  });
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // auto → high
  EXPECT_EQ(f.options.quality.tier, "high");
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // high → mid
  EXPECT_EQ(f.options.quality.tier, "mid");
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // mid → low
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // low → auto(回卷)
  EXPECT_EQ(f.options.quality.tier, "auto");
}
