#include "auth/AuthFactory.h"

namespace cnk {

namespace {

// 最小桩：仅 Name() 正确，Login() 明确返回未实现。
// 任务 11-21 会逐个用真实实现替换对应 Make*Auth，
// 本文件随之收缩，最终（任务 21）整体删除。
class StubAuth : public IAuth {
public:
    StubAuth(const char* name) : name_(name) {}
    LoginResult Login(const Profile&, const NetContext&) override {
        LoginResult r;
        r.message = std::string(name_) + " 尚未实现";
        return r;
    }
    const char* Name() const override { return name_; }
private:
    const char* name_;
};

}  // namespace

std::unique_ptr<IAuth> MakeDrcomAuth()        { return std::make_unique<StubAuth>("DrcomAuth"); }
std::unique_ptr<IAuth> MakeEportalAuth()      { return std::make_unique<StubAuth>("EportalAuth"); }
std::unique_ptr<IAuth> MakeSrunAuth()         { return std::make_unique<StubAuth>("SrunAuth"); }
std::unique_ptr<IAuth> MakeGenericFormAuth()  { return std::make_unique<StubAuth>("GenericFormAuth"); }
std::unique_ptr<IAuth> MakeCustomTemplateAuth(){ return std::make_unique<StubAuth>("CustomTemplateAuth"); }

}  // namespace cnk
