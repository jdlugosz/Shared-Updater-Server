#pragma once

#include "updater/atom.hpp"
#include "updater/persistence.hpp"

namespace updater {

class Processor {
public:
    virtual ~Processor() = default;
    virtual Result<void> initialize() = 0;
    virtual Reply process(const CheckRequest&, const RequestContext&, const std::function<bool()>&) = 0;
    virtual Result<void> activate(std::unique_ptr<const Catalog>) = 0;
    virtual Result<bool> maintain(WallTime, const Retention&) = 0;
    virtual std::string revision() const = 0;
};

class CheckProcessor final : public Processor {
public:
    CheckProcessor(std::unique_ptr<const Catalog> catalog, FeedOptions options,
                   std::unique_ptr<ContactSink> log, std::unique_ptr<CensusSink> census);
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Reply process(const CheckRequest& request, const RequestContext& context,
                                const std::function<bool()>& enter_persistence) override;
    // Candidates have already passed TOML validation and Atom preflight on the loader.
    [[nodiscard]] Result<void> activate(std::unique_ptr<const Catalog> candidate) override;
    [[nodiscard]] Result<bool> maintain(WallTime now, const Retention& retention) override;
    [[nodiscard]] std::string revision() const override;
private:
    std::unique_ptr<const Catalog> catalog_;
    FeedOptions options_;
    std::unique_ptr<ContactSink> log_;
    std::unique_ptr<CensusSink> census_;
};

} // namespace updater
