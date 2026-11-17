/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "pnext_chain.hpp"
#include "presentation_policy.hpp"

#include <array>
#include <limits>

namespace mako::layer {

#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    /// Owns only an explicitly negotiated, application-unused timing namespace.
    /// All storage and query work are bounded; no wait, polling loop or worker.
    class OwnedPresentTiming {
    public:
        using Clock = OrderedPresentTimeline::Clock;
        using TimePoint = OrderedPresentTimeline::TimePoint;
        static constexpr uint32_t capacity = 64;
        static constexpr size_t maxPresentStages = 2;
        static constexpr VkPresentStageFlagsEXT queueStage =
            VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
        static constexpr VkPresentStageFlagsEXT pixelOutStage =
            VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
        static constexpr VkPresentStageFlagsEXT pixelVisibleStage =
            VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
        static constexpr VkPresentStageFlagsEXT supportedStages =
            queueStage | pixelOutStage | pixelVisibleStage;

        struct Functions {
            PFN_vkSetSwapchainPresentTimingQueueSizeEXT setSize{};
            PFN_vkGetSwapchainTimingPropertiesEXT properties{};
            PFN_vkGetSwapchainTimeDomainPropertiesEXT domains{};
            PFN_vkGetPastPresentationTimingEXT past{};
            PFN_vkGetCalibratedTimestampsKHR calibrate{};
        };

        [[nodiscard]] static bool ownsNamespace(const VkDeviceCreateInfo& info) {
            if (applicationOwnsPresentationTiming(info, true))
                return false;
            // Even disabled caller feature nodes must not be duplicated.
            for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext);
                    node; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT ||
                        node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR)
                    return false;
            }
            return true;
        }

        // Feedback and absolute scheduling are independent surface capabilities.
        // Mesa Wayland can report IMAGE_FIRST_PIXEL_OUT while lacking commit-timing
        // support for absolute presentation targets.
        [[nodiscard]] static bool surfaceSupported(
                const VkPresentTimingSurfaceCapabilitiesEXT& timing,
                const VkSurfaceCapabilitiesPresentId2KHR& ids) {
            return ids.presentId2Supported && timing.presentTimingSupported &&
                (timing.presentStageQueries & supportedStages) != 0;
        }

        bool initialize(const VkDevice device, const VkSwapchainKHR swapchain,
                const Functions functions, const bool absoluteTimeSupported = true,
                const VkPresentStageFlagsEXT surfaceStages = queueStage) {
            this->device = device;
            this->swapchain = swapchain;
            this->functions = functions;
            this->absoluteTimeSupported = absoluteTimeSupported;
            const auto availableStages = surfaceStages & supportedStages;
            this->queueStageAvailable = (availableStages & queueStage) != 0;
            if (availableStages & pixelVisibleStage)
                this->feedbackStage = pixelVisibleStage;
            else if (availableStages & pixelOutStage)
                this->feedbackStage = pixelOutStage;
            else if (this->queueStageAvailable)
                this->feedbackStage = queueStage;
            else
                return false;
            this->outputFeedbackAvailable =
                this->feedbackStage == pixelVisibleStage ||
                this->feedbackStage == pixelOutStage;

            this->enabled = functions.setSize && functions.properties &&
                functions.domains && functions.past && functions.calibrate;
            if (!this->enabled)
                return false;
            this->enabled = this->refreshDomain() &&
                functions.setSize(device, swapchain, capacity) == VK_SUCCESS &&
                this->refreshProperties();
            return this->enabled;
        }

        [[nodiscard]] bool active() const { return this->enabled; }
        [[nodiscard]] bool controlsOutput(const double fallbackRefresh = 0) const {
            return this->enabled && this->mode == VK_PRESENT_MODE_FIFO_KHR &&
                OrderedPresentTimeline::validRate(this->refreshRate(fallbackRefresh));
        }
        [[nodiscard]] bool readinessFeedbackAvailable() const {
            return this->queueStageAvailable &&
                (this->feedbackStage == queueStage ||
                 this->domain != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
        }
        void observePresentMode(const void* chain) {
            for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
                if (node->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT)
                    continue;
                const auto* modes = reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(node);
                if (modes->swapchainCount == 1 && modes->pPresentModes)
                    this->mode = modes->pPresentModes[0];
            }
        }
        [[nodiscard]] uint32_t outstanding() const { return this->pendingCount; }

        /// A zero/unknown refresh is never invented. Variable refresh keeps
        /// the externally known panel ceiling; duration includes compositor caps.
        [[nodiscard]] double outputRate(const double fallback) const {
            const double rate = this->rate(this->refreshDuration);
            return rate > 0 ? (fallback > 0 ? std::min(fallback, rate) : rate) : fallback;
        }
        [[nodiscard]] double refreshRate(const double fallback) const {
            const double rate = this->rate(this->refreshInterval);
            return rate > 0 ? rate : this->outputRate(fallback);
        }

        /// Query once per source batch. Missing feedback fills only our fixed
        /// reservation set: future presents then request no new records.
        void update(OrderedPresentTimeline& timeline, const uint64_t batch) {
            if (!this->enabled || this->lastBatch == batch)
                return;
            this->lastBatch = batch;
            std::array<VkPastPresentationTimingEXT, capacity> records{};
            std::array<VkPresentStageTimeEXT, capacity * maxPresentStages> stages{};
            for (size_t i = 0; i < records.size(); ++i)
                records[i] = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
                    nullptr, 0, 0, static_cast<uint32_t>(maxPresentStages),
                    &stages[i * maxPresentStages], {}, 0, VK_FALSE};
            VkPastPresentationTimingPropertiesEXT result{
                .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
                .presentationTimingCount = capacity,
                .pPresentationTimings = records.data(),
            };
            const VkPastPresentationTimingInfoEXT query{
                .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
                .flags = VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT,
                .swapchain = this->swapchain,
            };
            const auto status = this->functions.past(this->device, &query, &result);
            if (status != VK_SUCCESS && status != VK_INCOMPLETE) {
                this->enabled = false;
                timeline.useUnobservedTiming();
                return;
            }
            const auto now = Clock::now();
            for (uint32_t i = 0; i < std::min(result.presentationTimingCount, capacity); ++i) {
                const auto& record = records[i];
                const auto slot = std::ranges::find(this->pendingIds, record.presentId);
                if (!record.presentId || slot == this->pendingIds.end() || !record.reportComplete)
                    continue;
                const auto offset = this->pendingOffsets[static_cast<size_t>(slot - this->pendingIds.begin())];
                TimePoint queueEnd{}, outputAt{};
                const bool validDomain =
                    result.timeDomainsCounter == this->domainCounter &&
                    record.timeDomain == this->domain &&
                    record.timeDomainId == this->domainId;
                if (validDomain) {
                    for (uint32_t stageIndex = 0;
                            stageIndex < std::min(record.presentStageCount,
                                static_cast<uint32_t>(maxPresentStages));
                            ++stageIndex) {
                        const auto& stage = record.pPresentStages[stageIndex];
                        if (!stage.time)
                            continue;
                        const auto sampledAt = this->fromDomain(stage.time, offset);
                        if (stage.stage == queueStage)
                            queueEnd = sampledAt;
                        if (this->outputFeedbackAvailable &&
                                stage.stage == this->feedbackStage)
                            outputAt = sampledAt;
                    }
                }
                *slot = 0;
                --this->pendingCount;
                // Pixel-out/visible feedback remains useful when the WSI cannot
                // schedule absolute targets. It lets the shared FIFO timeline
                // detect a persistent whole-refresh backlog and realign itself.
                timeline.feedbackTiming(record.presentId, queueEnd, now, outputAt);
            }
            if (result.timeDomainsCounter != this->domainCounter ||
                    now - this->lastCalibration >= std::chrono::seconds(1)) {
                // Never reinterpret queued records through a changed clock.
                if (result.timeDomainsCounter != this->domainCounter)
                    timeline.invalidateReadiness();
                this->enabled = this->refreshDomain();
                if (!this->enabled)
                    timeline.useUnobservedTiming();
            }
            if (this->enabled && result.timingPropertiesCounter != this->propertiesCounter &&
                    !this->refreshProperties()) {
                this->enabled = false;
                timeline.useUnobservedTiming();
            }
        }

        /// Keep software pacing in charge when absolute targets are unavailable.
        /// targetTime=0 requests ASAP presentation while still asking WSI for
        /// actual output-stage timestamps.
        bool prepare(const TimePoint target, OrderedPresentTimeline& timeline,
                VkPresentTimingInfoEXT& timing, uint64_t& id) {
            if (!this->enabled || this->nextId == UINT64_MAX)
                return false;
            const auto stamp = this->absoluteTimeSupported ? this->toDomain(target) : 0;
            if (this->absoluteTimeSupported && !stamp)
                return false;
            id = ++this->nextId;
            timing = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
                .targetTime = stamp,
                .timeDomainId = this->domainId,
                .targetTimeDomainPresentStage = this->feedbackStage,
            };
            if (this->pendingCount < capacity) {
                const auto slot = std::ranges::find(this->pendingIds, uint64_t{0});
                this->pendingOffsets[static_cast<size_t>(slot - this->pendingIds.begin())] = this->offset;
                *slot = id;
                ++this->pendingCount;
                timing.presentStageQueries = this->stageQueriesForCurrentDomain();
                timeline.submittedTiming(id, target, Clock::now());
            }
            return true;
        }

    private:
        [[nodiscard]] VkPresentStageFlagsEXT stageQueriesForCurrentDomain() const {
            VkPresentStageFlagsEXT stages = this->feedbackStage;
            // Stage-local clocks are calibrated against feedbackStage and cannot
            // safely describe a second, different stage in the same time domain.
            if (this->queueStageAvailable && this->feedbackStage != queueStage &&
                    this->domain != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT)
                stages |= queueStage;
            return stages;
        }

        static double rate(const uint64_t duration) {
            return duration >= 1'000'000 && duration <= 1'000'000'000
                ? 1e9 / static_cast<double>(duration) : 0;
        }
        static int64_t nanos(const TimePoint time) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        }
        uint64_t toDomain(const TimePoint time) const {
            const auto ns = nanos(time);
            if (ns < 0 || (this->offset > 0 && ns > INT64_MAX - this->offset) ||
                    (this->offset < 0 && ns < -this->offset))
                return 0;
            return static_cast<uint64_t>(ns + this->offset);
        }
        static TimePoint fromDomain(const uint64_t time, const int64_t offset) {
            if (!time || time > INT64_MAX ||
                    (offset < 0 && time > static_cast<uint64_t>(INT64_MAX + offset)) ||
                    (offset > 0 && time < static_cast<uint64_t>(offset)))
                return {};
            return TimePoint{std::chrono::nanoseconds(static_cast<int64_t>(time) - offset)};
        }
        bool refreshDomain() {
            std::array<VkTimeDomainKHR, 8> domains{};
            std::array<uint64_t, 8> ids{};
            VkSwapchainTimeDomainPropertiesEXT properties{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
                .timeDomainCount = static_cast<uint32_t>(domains.size()),
                .pTimeDomains = domains.data(),
                .pTimeDomainIds = ids.data(),
            };
            const auto status = this->functions.domains(this->device, this->swapchain,
                &properties, &this->domainCounter);
            if (status != VK_SUCCESS && status != VK_INCOMPLETE)
                return false;
            for (uint32_t i = 0; i < std::min(properties.timeDomainCount, uint32_t{8}); ++i) {
                if (domains[i] != VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR &&
                        domains[i] != VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT &&
                        domains[i] != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT)
                    continue;
                const VkSwapchainCalibratedTimestampInfoEXT swapchainInfo{
                    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
                    .swapchain = this->swapchain,
                    .presentStage = domains[i] ==
                            VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT
                        ? this->feedbackStage : VkPresentStageFlagsEXT{0},
                    .timeDomainId = ids[i],
                };
                const VkCalibratedTimestampInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
                    .pNext = domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR ? nullptr : &swapchainInfo,
                    .timeDomain = domains[i],
                };
                uint64_t timestamp{}, deviation{};
                const auto before = Clock::now();
                const auto calibrated = this->functions.calibrate(this->device, 1, &info,
                    &timestamp, &deviation);
                const auto after = Clock::now();
                if (calibrated != VK_SUCCESS || timestamp > INT64_MAX || deviation > 1'000'000 ||
                        after < before || nanos(after) - nanos(before) > static_cast<int64_t>(1'000'000 - deviation))
                    continue;
                this->offset = static_cast<int64_t>(timestamp) - nanos(before + (after - before) / 2);
                this->domain = domains[i];
                this->domainId = ids[i];
                this->lastCalibration = after;
                return true;
            }
            return false;
        }
        bool refreshProperties() {
            VkSwapchainTimingPropertiesEXT properties{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT,
            };
            uint64_t counter{};
            const auto status = this->functions.properties(this->device, this->swapchain, &properties, &counter);
            if (status == VK_NOT_READY) {
                // Some WSI implementations need a present before publishing
                // initial or changed properties. Forget stale limits and retry
                // on the next source batch, even if its counter is unchanged.
                this->refreshDuration = this->refreshInterval = 0;
                this->propertiesCounter.reset();
                return true;
            }
            if (status != VK_SUCCESS)
                return false;
            this->refreshDuration = properties.refreshDuration;
            this->refreshInterval = properties.refreshInterval;
            this->propertiesCounter = counter;
            return true;
        }

        VkDevice device{};
        VkSwapchainKHR swapchain{};
        Functions functions;
        bool enabled{};
        VkPresentModeKHR mode{VK_PRESENT_MODE_FIFO_KHR};
        bool absoluteTimeSupported{};
        bool queueStageAvailable{};
        bool outputFeedbackAvailable{};
        VkPresentStageFlagsEXT feedbackStage{};
        VkTimeDomainKHR domain{};
        uint64_t domainId{}, domainCounter{};
        std::optional<uint64_t> propertiesCounter;
        uint64_t refreshDuration{}, refreshInterval{}, nextId{};
        std::optional<uint64_t> lastBatch;
        TimePoint lastCalibration{};
        int64_t offset{};
        std::array<uint64_t, capacity> pendingIds{};
        std::array<int64_t, capacity> pendingOffsets{};
        uint32_t pendingCount{};
    };

#else
    class OwnedPresentTiming {
    public:
        [[nodiscard]] bool active() const { return false; }
        [[nodiscard]] bool controlsOutput(double = 0) const { return false; }
    };
#endif
}
