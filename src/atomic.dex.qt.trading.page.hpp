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

#pragma once

//! QT
#include <QObject>

//! PCH
#include "atomic.dex.pch.hpp"

//! Project Headers
#include "atomic.dex.events.hpp"
#include "atomic.dex.qt.actions.hpp"
#include "atomic.dex.qt.candlestick.charts.model.hpp"
#include "atomic.dex.qt.market.pairs.hpp"
#include "atomic.dex.qt.orderbook.hpp"
#include "atomic.dex.qt.portfolio.model.hpp"

namespace atomic_dex
{
    class trading_page final : public QObject, public ag::ecs::pre_update_system<trading_page>
    {
        //! Q_Object definition
        Q_OBJECT

        //! Q Properties definitions
        Q_PROPERTY(qt_orderbook_wrapper* orderbook READ get_orderbook_wrapper NOTIFY orderbookChanged)
        Q_PROPERTY(candlestick_charts_model* candlestick_charts_mdl READ get_candlestick_charts NOTIFY candlestickChartsChanged)
        Q_PROPERTY(market_pairs* market_pairs_mdl READ get_market_pairs_mdl NOTIFY marketPairsChanged)

        //! Private enum
        enum models
        {
            orderbook       = 0,
            ohlc            = 1,
            market_selector = 2,
            models_size     = 3
        };

        enum models_actions
        {
            candlestick_need_a_reset = 0,
            orderbook_need_a_reset   = 1,
            models_actions_size      = 2
        };

        enum class trading_actions
        {
            post_process_orderbook_finished = 0,
            refresh_ohlc                    = 1,
        };

        //! Private typedefs
        using t_models         = std::array<QObject*, models_size>;
        using t_models_actions = std::array<std::atomic_bool, models_actions_size>;
        using t_actions_queue  = boost::lockfree::queue<trading_actions>;

        //! Private members fields
        ag::ecs::system_manager& m_system_manager;
        std::atomic_bool&        m_about_to_exit_the_app;
        t_models                 m_models;
        t_models_actions         m_models_actions;
        t_actions_queue          m_actions_queue{g_max_actions_size};

      public:
        //! Constructor
        explicit trading_page(
            entt::registry& registry, ag::ecs::system_manager& system_manager, std::atomic_bool& exit_status, portfolio_model* portfolio,
            QObject* parent = nullptr);
        ~trading_page() noexcept final = default;

        //! Public override
        void update() noexcept final;

        //! Public API
        void process_action();
        void connect_signals();
        void disconnect_signals();
        void clear_models();
        void disable_coin(const QString& coin) noexcept;;

        //! Public QML API
        Q_INVOKABLE void    set_current_orderbook(const QString& base, const QString& rel);
        Q_INVOKABLE void    on_gui_enter_dex();
        Q_INVOKABLE void    on_gui_leave_dex();
        Q_INVOKABLE void    cancel_order(const QString& order_id);
        Q_INVOKABLE void    cancel_all_orders();
        Q_INVOKABLE void    cancel_all_orders_by_ticker(const QString& ticker);
        Q_INVOKABLE QString place_buy_order(
            const QString& base, const QString& rel, const QString& price, const QString& volume, bool is_created_order, const QString& price_denom,
            const QString& price_numer, const QString& base_nota = "", const QString& base_confs = "");
        Q_INVOKABLE QString place_sell_order(
            const QString& base, const QString& rel, const QString& price, const QString& volume, bool is_created_order, const QString& price_denom,
            const QString& price_numer, const QString& rel_nota = "", const QString& rel_confs = "");
        Q_INVOKABLE void swap_market_pair();
        Q_INVOKABLE QVariant get_raw_mm2_coin_cfg(const QString& ticker) const noexcept;

        //! Properties
        [[nodiscard]] qt_orderbook_wrapper*     get_orderbook_wrapper() const noexcept;
        [[nodiscard]] candlestick_charts_model* get_candlestick_charts() const noexcept;
        [[nodiscard]] market_pairs*             get_market_pairs_mdl() const noexcept;

        //! Events Callbacks
        void on_process_orderbook_finished_event(const process_orderbook_finished& evt) noexcept;
        void on_start_fetching_new_ohlc_data_event(const start_fetching_new_ohlc_data& evt);
        void on_refresh_ohlc_event(const refresh_ohlc_needed& evt) noexcept;

      signals:
        void orderbookChanged();
        void candlestickChartsChanged();
        void marketPairsChanged();
    };
} // namespace atomic_dex

REFL_AUTO(type(atomic_dex::trading_page))