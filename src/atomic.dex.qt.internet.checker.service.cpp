/******************************************************************************
 * Copyright © 2013-2019 The Komodo Platform Developers.                      *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * Komodo Platform software, including this file may be copied, modified,     *
 * propagated or distributed except according to the terms contained in the   *
 * LICENSE file                                                               *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

//! Our project
#include "atomic.dex.qt.internet.checker.service.hpp"
#include "atomic.dex.qt.utilities.hpp"
#include "atomic.threadpool.hpp"

//! QT Properties
namespace atomic_dex
{
    void
    atomic_dex::internet_service_checker::set_internet_alive(bool internet_status) noexcept
    {
        if (internet_status != is_internet_reacheable)
        {
            is_internet_reacheable = internet_status;
            emit internetStatusChanged();
        }
    }

    bool
    atomic_dex::internet_service_checker::is_internet_alive() const noexcept
    {
        return is_internet_reacheable.load();
    }

    double
    atomic_dex::internet_service_checker::get_seconds_left_to_auto_retry() const noexcept
    {
        return m_timer;
    }

    void
    atomic_dex::internet_service_checker::set_seconds_left_to_auto_retry(double time_left) noexcept
    {
        m_timer = time_left;
        emit secondsLeftToAutoRetryChanged();
    }
} // namespace atomic_dex

namespace atomic_dex
{
    internet_service_checker::internet_service_checker(entt::registry& registry, QObject* parent) : QObject(parent), system(registry)
    {
        //! Init
        retry();
    }

    void
    atomic_dex::internet_service_checker::retry() noexcept
    {
        using namespace std::chrono_literals;
        m_update_clock = std::chrono::high_resolution_clock::now();
        set_seconds_left_to_auto_retry(15.0);
        this->fetch_internet_connection();
    }

    void
    internet_service_checker::update() noexcept
    {
        using namespace std::chrono_literals;

        const auto now = std::chrono::high_resolution_clock::now();
        const auto s   = std::chrono::duration_cast<std::chrono::seconds>(now - m_update_clock);
        set_seconds_left_to_auto_retry(15.0 - s.count());
        if (s >= 15s)
        {
            this->fetch_internet_connection();
            m_update_clock = std::chrono::high_resolution_clock::now();
            set_seconds_left_to_auto_retry(15.0);
        }
    }

    void
    internet_service_checker::fetch_internet_connection()
    {
        spdlog::info("fetching internet status begin");
        spawn([this]() {
            is_google_reacheable               = am_i_able_to_reach_this_endpoint("https://www.google.com");
            is_paprika_provider_alive          = am_i_able_to_reach_this_endpoint("https://api.coinpaprika.com/v1/coins/btc-bitcoin");
            is_our_private_endpoint_reacheable = am_i_able_to_reach_this_endpoint("https://komodo.live:3333/api/v1/ohlc/tickers_list");
            bool res                           = is_google_reacheable || is_paprika_provider_alive || is_our_private_endpoint_reacheable;
            this->set_internet_alive(res);
            spdlog::info("fetching internet status finished, internet status is: {}", res);
        });
    }
} // namespace atomic_dex