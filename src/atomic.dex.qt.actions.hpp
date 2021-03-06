#pragma once

namespace atomic_dex
{
    //! Possible front end actions
    enum class action
    {
        refresh_enabled_coin             = 0,
        refresh_current_ticker           = 1,
        refresh_transactions             = 2,
        refresh_portfolio_ticker_balance = 3,
        refresh_update_status            = 4,
        post_process_orders_finished     = 5,
        post_process_swaps_finished      = 6,
    };

    inline constexpr std::size_t g_max_actions_size{128};
} // namespace atomic_dex