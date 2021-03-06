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

//! Project Headers
#include "atomic.dex.mm2.hpp"
#include "atomic.dex.kill.hpp"
#include "atomic.dex.mm2.config.hpp"
#include "atomic.dex.security.hpp"
#include "atomic.dex.version.hpp"
#include "atomic.threadpool.hpp"

//! Anonymous functions
namespace
{
    namespace ag = antara::gaming;

    void
    check_for_reconfiguration(const std::string& wallet_name)
    {
        using namespace std::string_literals;
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        fs::path    cfg_path                   = get_atomic_dex_config_folder();
        std::string filename                   = std::string(atomic_dex::get_precedent_raw_version()) + "-coins." + wallet_name + ".json";
        fs::path    precedent_version_cfg_path = cfg_path / filename;

        if (fs::exists(precedent_version_cfg_path))
        {
            //! There is a precedent configuration file
            spdlog::info("There is a precedent configuration file, upgrading the new one with precedent settings");

            //! Old cfg to ifs
            std::ifstream ifs(precedent_version_cfg_path.string());
            assert(ifs.is_open());
            nlohmann::json precedent_config_json_data;
            ifs >> precedent_config_json_data;

            //! New cfg to ifs
            fs::path      actual_version_filepath = cfg_path / (std::string(atomic_dex::get_raw_version()) + "-coins."s + wallet_name + ".json"s);
            std::ifstream actual_version_ifs(actual_version_filepath.string());
            assert(actual_version_ifs.is_open());
            nlohmann::json actual_config_data;
            actual_version_ifs >> actual_config_data;

            //! Iterate through new config
            for (auto& [key, value]: actual_config_data.items())
            {
                //! If the coin in new config is present in the old one, copy the contents
                if (precedent_config_json_data.contains(key))
                {
                    actual_config_data.at(key)["active"] = precedent_config_json_data.at(key).at("active").get<bool>();
                }
            }

            ifs.close();
            actual_version_ifs.close();

            //! Write contents
            std::ofstream ofs(actual_version_filepath.string());
            assert(ofs.is_open());
            ofs << actual_config_data;

            //! Delete old cfg
            boost::system::error_code ec;
            fs::remove(precedent_version_cfg_path, ec);
            if (ec)
            {
                spdlog::error("error: {}", ec.message());
            }
        }
    }

    void
    update_coin_status(const std::string& wallet_name, const std::vector<std::string> tickers, bool status = true)
    {
        fs::path       cfg_path = get_atomic_dex_config_folder();
        std::string    filename = std::string(atomic_dex::get_raw_version()) + "-coins." + wallet_name + ".json";
        std::ifstream  ifs((cfg_path / filename).c_str());
        nlohmann::json config_json_data;

        assert(ifs.is_open());
        ifs >> config_json_data;

        for (auto&& ticker: tickers) { config_json_data.at(ticker)["active"] = status; }

        ifs.close();

        //! Write contents
        std::ofstream ofs((cfg_path / filename).c_str(), std::ios::trunc);
        assert(ofs.is_open());
        ofs << config_json_data;
    }

    bool
    retrieve_coins_information(const std::string& wallet_name, atomic_dex::t_coins_registry& coins_registry)
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        check_for_reconfiguration(wallet_name);
        const auto  cfg_path = get_atomic_dex_config_folder();
        std::string filename = std::string(atomic_dex::get_raw_version()) + "-coins." + wallet_name + ".json";
        spdlog::info("Retrieving Wallet information of {}", (cfg_path / filename).string());
        if (exists(cfg_path / filename))
        {
            std::ifstream ifs((cfg_path / filename).c_str());
            assert(ifs.is_open());
            nlohmann::json config_json_data;
            ifs >> config_json_data;
            auto res = config_json_data.get<std::unordered_map<std::string, atomic_dex::coin_config>>();
            for (auto&& [key, value]: res) { coins_registry.insert_or_assign(key, value); }
            return true;
        }
        return false;
    }
} // namespace

namespace atomic_dex
{
    mm2::mm2(entt::registry& registry) : system(registry)
    {
        m_orderbook_clock = std::chrono::high_resolution_clock::now();
        m_info_clock      = std::chrono::high_resolution_clock::now();

        dispatcher_.sink<gui_enter_trading>().connect<&mm2::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().connect<&mm2::on_gui_leave_trading>(*this);
        dispatcher_.sink<orderbook_refresh>().connect<&mm2::on_refresh_orderbook>(*this);

        m_swaps_registry.insert("result", t_my_recent_swaps_answer{.limit = 0, .total = 0});
    }

    void
    mm2::update() noexcept
    {
        using namespace std::chrono_literals;

        if (not m_mm2_running)
        {
            return;
        }

        const auto now    = std::chrono::high_resolution_clock::now();
        const auto s      = std::chrono::duration_cast<std::chrono::seconds>(now - m_orderbook_clock);
        const auto s_info = std::chrono::duration_cast<std::chrono::seconds>(now - m_info_clock);

        if (s >= 5s)
        {
            spawn([this]() { fetch_current_orderbook_thread(false); });
            spawn([this]() {
                std::vector<std::future<void>> futures;
                futures.emplace_back(spawn([this]() { process_orders(); }));
                futures.emplace_back(spawn([this]() { process_swaps(); }));
                for (auto&& fut: futures) { fut.get(); }
            });
            m_orderbook_clock = std::chrono::high_resolution_clock::now();
        }

        if (s_info >= 30s)
        {
            spawn([this]() { fetch_infos_thread(); });
            m_info_clock = std::chrono::high_resolution_clock::now();
        }
    }

    mm2::~mm2() noexcept
    {
        m_mm2_running = false;

#if defined(_WIN32) || defined(WIN32)
        atomic_dex::kill_executable("mm2");
#else
        const reproc::stop_actions stop_actions = {
            {reproc::stop::terminate, reproc::milliseconds(2000)},
            {reproc::stop::kill, reproc::milliseconds(5000)},
            {reproc::stop::wait, reproc::milliseconds(2000)}};

        const auto ec = m_mm2_instance.stop(stop_actions).second;

        if (ec)
        {
            spdlog::error("error: {}", ec.message());
        }
#endif

        if (m_mm2_init_thread.joinable())
        {
            m_mm2_init_thread.join();
        }

        dispatcher_.sink<gui_enter_trading>().disconnect<&mm2::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().disconnect<&mm2::on_gui_leave_trading>(*this);
        dispatcher_.sink<orderbook_refresh>().disconnect<&mm2::on_refresh_orderbook>(*this);
    }

    const std::atomic_bool&
    mm2::is_mm2_running() const noexcept
    {
        return m_mm2_running;
    }

    t_coins
    mm2::get_all_coins() const noexcept
    {
        t_coins destination;

        destination.reserve(m_coins_informations.size());
        for (auto&& [key, value]: m_coins_informations)
        {
            //!
            destination.push_back(value);
        }

        std::sort(begin(destination), end(destination), [](auto&& lhs, auto&& rhs) { return lhs.ticker < rhs.ticker; });

        return destination;
    }

    t_coins
    mm2::get_enabled_coins() const noexcept
    {
        t_coins destination;

        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.currently_enabled)
            {
                destination.push_back(value);
            }
        }

        std::sort(begin(destination), end(destination), [](auto&& lhs, auto&& rhs) { return lhs.ticker < rhs.ticker; });

        return destination;
    }

    t_coins
    mm2::get_enableable_coins() const noexcept
    {
        t_coins destination;

        for (auto&& [key, value]: m_coins_informations)
        {
            if (not value.currently_enabled)
            {
                destination.emplace_back(value);
            }
        }

        return destination;
    }

    t_coins
    mm2::get_active_coins() const noexcept
    {
        t_coins destination;

        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.active)
            {
                destination.emplace_back(value);
            }
        }

        return destination;
    }

    bool
    mm2::disable_coin(const std::string& ticker, std::error_code& ec) noexcept
    {
        coin_config coin_info = m_coins_informations.at(ticker);
        if (not coin_info.currently_enabled)
        {
            return true;
        }

        t_disable_coin_request request{.coin = ticker};
        auto                   answer = rpc_disable_coin(std::move(request));

        if (answer.error.has_value())
        {
            std::string error = answer.error.value();
            if (error.find("such coin") != std::string::npos)
            {
                ec = dextop_error::disable_unknown_coin;
                return false;
            }
            else if (error.find("active swaps") != std::string::npos)
            {
                ec = dextop_error::active_swap_is_using_the_coin;
                return false;
            }
            else if (error.find("matching orders") != std::string::npos)
            {
                ec = dextop_error::order_is_matched_at_the_moment;
                return false;
            }
        }

        coin_info.currently_enabled = false;
        m_coins_informations.assign(coin_info.ticker, coin_info);

        dispatcher_.trigger<coin_disabled>(ticker);
        return true;
    }

    bool
    mm2::enable_coin(const std::string& ticker, bool emit_event)
    {
        coin_config coin_info = m_coins_informations.at(ticker);

        if (coin_info.currently_enabled)
        {
            return true;
        }

        if (not coin_info.is_erc_20)
        {
            t_electrum_request request{.coin_name = coin_info.ticker, .servers = coin_info.electrum_urls.value(), .with_tx_history = true};
            const auto         answer = rpc_electrum(std::move(request));
            if (answer.result not_eq "success")
            {
                return false;
            }
        }
        else
        {
            t_enable_request request{.coin_name = coin_info.ticker, .urls = coin_info.eth_urls.value()};
            const auto       answer = rpc_enable(std::move(request));
            if (answer.result not_eq "success")
            {
                return false;
            }
        }


        coin_info.currently_enabled = true;
        m_coins_informations.assign(coin_info.ticker, coin_info);

        spawn([this, copy_ticker = ticker]() { process_balance(copy_ticker); });
        spawn([this, copy_ticker = ticker]() { process_tx(copy_ticker, false); });

        dispatcher_.trigger<coin_enabled>(ticker);
        if (emit_event)
        {
            this->dispatcher_.trigger<enabled_coins_event>();
        }
        return true;
    }

    bool
    mm2::enable_default_coins() noexcept
    {
        std::atomic<std::size_t> result{1};
        auto                     coins = get_active_coins();

        std::vector<std::string> tickers;
        tickers.reserve(coins.size());
        for (auto&& current_coin: coins) { tickers.push_back(current_coin.ticker); }

        batch_enable_coins(tickers);

        this->dispatcher_.trigger<enabled_default_coins_event>();

        spawn([this]() {
            process_orders();
            process_swaps();
        });

        return result.load() == 1;
    }

    void
    mm2::disable_multiple_coins(const std::vector<std::string>& tickers) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        for (const auto& ticker: tickers)
        {
            spawn([this, ticker]() {
                std::error_code ec;
                disable_coin(ticker, ec);
                if (ec)
                {
                    spdlog::warn("{}", ec.message());
                }
            });
        }

        update_coin_status(this->m_current_wallet_name, tickers, false);
    }

    void
    mm2::batch_enable_coins(const std::vector<std::string>& tickers, bool emit_event) noexcept
    {
        std::vector<t_electrum_request> requests;
        std::vector<t_enable_request>   requests_erc;

        for (const auto& ticker: tickers)
        {
            coin_config coin_info = m_coins_informations.at(ticker);

            if (coin_info.currently_enabled)
            {
                continue;
            }

            if (not coin_info.is_erc_20)
            {
                t_electrum_request request{.coin_name = coin_info.ticker, .servers = coin_info.electrum_urls.value(), .with_tx_history = true};
                requests.emplace_back(request);
            }
            else
            {
                t_enable_request request{.coin_name = coin_info.ticker, .urls = coin_info.eth_urls.value(), .with_tx_history = true};
                requests_erc.emplace_back(request);
            }
        }

        auto functor_process_answer = [this, &emit_event](const nlohmann::json& answer) {
            if (answer.count("coin") == 1)
            {
                auto        ticker          = answer.at("coin").get<std::string>();
                coin_config coin_info       = m_coins_informations.at(ticker);
                coin_info.currently_enabled = true;
                m_coins_informations.assign(coin_info.ticker, coin_info);

                process_balance(ticker);
                process_tx(ticker, false);

                dispatcher_.trigger<coin_enabled>(ticker);
                if (emit_event)
                {
                    this->dispatcher_.trigger<enabled_coins_event>();
                }
            }
        };

        if (not requests.empty())
        {
            auto answers = rpc_batch_electrum(requests);
            if (answers.count("error") == 0)
            {
                for (auto&& answer: answers) { functor_process_answer(answer); }
            }
        }

        if (not requests_erc.empty())
        {
            auto answers_erc = rpc_batch_enable(requests_erc);
            if (answers_erc.count("error") == 0)
            {
                for (auto&& answer_erc: answers_erc) { functor_process_answer(answer_erc); }
            }
        }
    }

    void
    mm2::enable_multiple_coins(const std::vector<std::string>& tickers) noexcept
    {
        spawn([this, tickers]() { batch_enable_coins(tickers, true); });

        update_coin_status(this->m_current_wallet_name, tickers, true);
    }

    coin_config
    mm2::get_coin_info(const std::string& ticker) const
    {
        if (m_coins_informations.find(ticker) == m_coins_informations.cend())
        {
            return {};
        }
        return m_coins_informations.at(ticker);
    }

    t_orderbook_answer
    mm2::get_orderbook(t_mm2_ec& ec) const noexcept
    {
        auto&& [base, rel]     = this->m_synchronized_ticker_pair.get();
        const std::string pair = base + "/" + rel;
        if (m_current_orderbook.empty())
        {
            ec = dextop_error::orderbook_empty;
            return {};
        }
        if (m_current_orderbook.find(pair) == m_current_orderbook.cend())
        {
            ec = dextop_error::orderbook_ticker_not_found;
            return {};
        }
        return m_current_orderbook.at(pair);
    }

    void
    mm2::batch_process_fees_and_fetch_current_orderbook_thread(bool is_a_reset)
    {
        spdlog::info("batch orderbook/fees");
        if (not m_orderbook_thread_active)
        {
            spdlog::warn("Nothing todo, sleeping");
            return;
        }

        //! Prepare fees
        auto&& [orderbook_ticker_base, orderbook_ticker_rel] = m_synchronized_ticker_pair.get();
        if (orderbook_ticker_rel.empty())
            return;
        nlohmann::json          batch = nlohmann::json::array();
        t_get_trade_fee_request req_base{.coin = orderbook_ticker_base};
        nlohmann::json          current_request = ::mm2::api::template_request("get_trade_fee");
        ::mm2::api::to_json(current_request, req_base);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("get_trade_fee");
        ;
        t_get_trade_fee_request req_rel{.coin = orderbook_ticker_rel};
        ::mm2::api::to_json(current_request, req_rel);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("orderbook");
        t_orderbook_request req_orderbook{.base = orderbook_ticker_base, .rel = orderbook_ticker_rel};
        ::mm2::api::to_json(current_request, req_orderbook);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("max_taker_vol");
        ::mm2::api::max_taker_vol_request req_base_max_taker_vol{.coin = orderbook_ticker_base};
        ::mm2::api::to_json(current_request, req_base_max_taker_vol);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("max_taker_vol");
        ::mm2::api::max_taker_vol_request req_rel_max_taker_vol{.coin = orderbook_ticker_rel};
        ::mm2::api::to_json(current_request, req_rel_max_taker_vol);
        batch.push_back(current_request);
        auto answer = ::mm2::api::rpc_batch_standalone(batch);

        if (answer.is_array())
        {
            auto trade_fee_base_answer = ::mm2::api::rpc_process_answer_batch<t_get_trade_fee_answer>(answer[0], "get_trade_fee");
            if (trade_fee_base_answer.rpc_result_code == 200)
            {
                this->m_trade_fees_registry.insert_or_assign(orderbook_ticker_base, trade_fee_base_answer);
            }

            auto trade_fee_rel_answer = ::mm2::api::rpc_process_answer_batch<t_get_trade_fee_answer>(answer[1], "get_trade_fee");
            if (trade_fee_rel_answer.rpc_result_code == 200)
            {
                this->m_trade_fees_registry.insert_or_assign(orderbook_ticker_rel, trade_fee_rel_answer);
            }

            auto orderbook_answer = ::mm2::api::rpc_process_answer_batch<t_orderbook_answer>(answer[2], "orderbook");

            if (orderbook_answer.rpc_result_code == 200)
            {
                m_current_orderbook.insert_or_assign(orderbook_ticker_base + "/" + orderbook_ticker_rel, orderbook_answer);
                this->dispatcher_.trigger<process_orderbook_finished>(is_a_reset);
            }

            auto base_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[3], "max_taker_vol");
            if (base_max_taker_vol_answer.rpc_result_code == 200)
            {
                this->m_synchronized_max_taker_vol->first         = base_max_taker_vol_answer.result.value();
                t_float_50 base_res                               = t_float_50(this->m_synchronized_max_taker_vol->first.decimal) * m_balance_factor;
                this->m_synchronized_max_taker_vol->first.decimal = base_res.str(8);
            }

            auto rel_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[4], "max_taker_vol");
            if (rel_max_taker_vol_answer.rpc_result_code == 200)
            {
                this->m_synchronized_max_taker_vol->second         = rel_max_taker_vol_answer.result.value();
                t_float_50 rel_res                                 = t_float_50(this->m_synchronized_max_taker_vol->second.decimal) * m_balance_factor;
                this->m_synchronized_max_taker_vol->second.decimal = rel_res.str(8);
            }
        }
    }

    void
    mm2::process_orderbook(bool is_a_reset)
    {
        auto&& [base, rel] = m_synchronized_ticker_pair.get();
        if (rel.empty())
            return;
        nlohmann::json batch = nlohmann::json::array();

        nlohmann::json      current_request = ::mm2::api::template_request("orderbook");
        t_orderbook_request req_orderbook{.base = base, .rel = rel};
        ::mm2::api::to_json(current_request, req_orderbook);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("max_taker_vol");
        ::mm2::api::max_taker_vol_request req_base_max_taker_vol{.coin = base};
        ::mm2::api::to_json(current_request, req_base_max_taker_vol);
        batch.push_back(current_request);
        current_request = ::mm2::api::template_request("max_taker_vol");
        ::mm2::api::max_taker_vol_request req_rel_max_taker_vol{.coin = rel};
        ::mm2::api::to_json(current_request, req_rel_max_taker_vol);
        batch.push_back(current_request);
        auto answer = ::mm2::api::rpc_batch_standalone(batch);
        if (answer.is_array())
        {
            auto orderbook_answer = ::mm2::api::rpc_process_answer_batch<t_orderbook_answer>(answer[0], "orderbook");

            if (orderbook_answer.rpc_result_code == 200)
            {
                m_current_orderbook.insert_or_assign(base + "/" + rel, orderbook_answer);
                this->dispatcher_.trigger<process_orderbook_finished>(is_a_reset);
            }

            auto base_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[1], "max_taker_vol");
            if (base_max_taker_vol_answer.rpc_result_code == 200)
            {
                this->m_synchronized_max_taker_vol->first         = base_max_taker_vol_answer.result.value();
                t_float_50 base_res                               = t_float_50(this->m_synchronized_max_taker_vol->first.decimal) * m_balance_factor;
                this->m_synchronized_max_taker_vol->first.decimal = base_res.str(8);
            }

            auto rel_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[2], "max_taker_vol");
            if (rel_max_taker_vol_answer.rpc_result_code == 200)
            {
                this->m_synchronized_max_taker_vol->second         = rel_max_taker_vol_answer.result.value();
                t_float_50 rel_res                                 = t_float_50(this->m_synchronized_max_taker_vol->second.decimal) * m_balance_factor;
                this->m_synchronized_max_taker_vol->second.decimal = rel_res.str(8);
            }
        }
    }

    void
    mm2::fetch_current_orderbook_thread(bool is_a_reset)
    {
        spdlog::info("Fetch current orderbook");

        //! If thread is not active ex: we are not on the trading page anymore, we continue sleeping.
        if (not m_orderbook_thread_active)
        {
            spdlog::warn("Nothing todo, sleeping");
            return;
        }

        process_orderbook(is_a_reset);
    }

    void
    mm2::fetch_infos_thread(bool is_a_refresh)
    {
        spdlog::info("{}: Fetching Infos l{}", __FUNCTION__, __LINE__);

        t_coins                        coins = get_enabled_coins();
        std::vector<std::future<void>> futures;

        futures.reserve(coins.size() * 2);

        for (auto&& current_coin: coins)
        {
            futures.emplace_back(spawn([this, ticker = current_coin.ticker]() { process_balance(ticker); }));
            futures.emplace_back(spawn([this, ticker = current_coin.ticker, is_a_refresh]() { process_tx(ticker, is_a_refresh); }));
        }

        for (auto&& fut: futures) { fut.get(); }
    }

    void
    mm2::spawn_mm2_instance(std::string wallet_name, std::string passphrase, bool with_pin_cfg)
    {
        this->m_balance_factor = determine_balance_factor(with_pin_cfg);
        spdlog::trace("balance factor is: {}", m_balance_factor);
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        this->m_current_wallet_name = std::move(wallet_name);
        retrieve_coins_information(this->m_current_wallet_name, m_coins_informations);
        mm2_config cfg{.passphrase = std::move(passphrase), .rpc_password = atomic_dex::gen_random_password()};
        ::mm2::api::set_rpc_password(cfg.rpc_password);
        json       json_cfg;
        const auto tools_path = ag::core::assets_real_path() / "tools/mm2/";

        nlohmann::to_json(json_cfg, cfg);
        fs::path mm2_cfg_path = (fs::temp_directory_path() / "MM2.json");

        std::ofstream ofs(mm2_cfg_path.string());
        ofs << json_cfg.dump();
        ofs.close();
        const std::array<std::string, 1> args = {(tools_path / "mm2").string()};
        reproc::options                  options;
        options.redirect.parent = true;
#if defined(WIN32)
        std::ostringstream env_mm2;
        env_mm2 << "MM_CONF_PATH=" << mm2_cfg_path.string();
        _putenv(env_mm2.str().c_str());
        spdlog::debug("env: {}", std::getenv("MM_CONF_PATH"));
#else
        options.environment = std::unordered_map<std::string, std::string>{{"MM_CONF_PATH", mm2_cfg_path.string()}};
#endif
        options.working_directory = strdup(tools_path.string().c_str());

        spdlog::debug("command line: {}, from directory: {}", args[0], options.working_directory);
        const auto ec = m_mm2_instance.start(args, options);
        std::free((void*)options.working_directory);
        if (ec)
        {
            spdlog::error("{}", ec.message());
        }

        m_mm2_init_thread = std::thread([this, mm2_cfg_path]() {
            using namespace std::chrono_literals;
            auto               check_mm2_alive = []() { return ::mm2::api::rpc_version() != "error occured during rpc_version"; };
            static std::size_t nb_try          = 0;

            while (not check_mm2_alive())
            {
                nb_try += 1;
                if (nb_try == 30)
                {
                    spdlog::error("MM2 not started correctly");
                    //! TODO: emit mm2_failed_initialization
                    fs::remove(mm2_cfg_path);
                    return;
                }
                std::this_thread::sleep_for(1s);
            }

            fs::remove(mm2_cfg_path);
            spdlog::info("mm2 is initialized");
            dispatcher_.trigger<mm2_initialized>();
            enable_default_coins();
            m_mm2_running = true;
            dispatcher_.trigger<mm2_started>();
        });
    }

    std::string
    mm2::my_balance_with_locked_funds(const std::string& ticker, t_mm2_ec& ec) const
    {
        if (m_balance_informations.find(ticker) == m_balance_informations.cend())
        {
            ec = dextop_error::balance_of_a_non_enabled_coin;
            return "0";
        }

        t_float_50 final_balance = get_balance(ticker);

        return final_balance.convert_to<std::string>();
    }

    t_float_50
    mm2::get_balance(const std::string& ticker) const
    {
        if (m_balance_informations.find(ticker) == m_balance_informations.end())
        {
            return 0;
        }

        const auto answer = m_balance_informations.at(ticker);
        t_float_50 balance(answer.balance);

        return balance;
    }

    t_transactions
    mm2::get_tx_history(const std::string& ticker, t_mm2_ec& ec) const
    {
        if (m_tx_informations.find(ticker) == m_tx_informations.cend())
        {
            ec = dextop_error::tx_history_of_a_non_enabled_coin;
            return {};
        }

        return m_tx_informations.at(ticker);
    }

    std::string
    mm2::my_balance(const std::string& ticker, t_mm2_ec& ec) const
    {
        if (m_balance_informations.find(ticker) == m_balance_informations.cend())
        {
            ec = dextop_error::balance_of_a_non_enabled_coin;
            return "0";
        }

        return m_balance_informations.at(ticker).balance;
    }

    t_withdraw_answer
    mm2::withdraw(t_withdraw_request&& request, t_mm2_ec& ec) noexcept
    {
        auto result = rpc_withdraw(std::move(request));
        if (result.error.has_value())
        {
            ec = dextop_error::rpc_withdraw_error;
        }
        if (result.raw_result.find("Not sufficient balance. Couldn't collect enough value from utxos") != std::string::npos)
        {
            result.error = "Not enough funds to cover txfee, please reduce amount.";
        }
        return result;
    }

    t_broadcast_answer
    mm2::broadcast(t_broadcast_request&& request, t_mm2_ec& ec) noexcept
    {
        std::string coin   = request.coin;
        auto        result = rpc_send_raw_transaction(std::move(request));
        if (result.rpc_result_code == -1)
        {
            ec = dextop_error::rpc_send_raw_transaction_error;
        }
        else
        {
            if (this->get_coin_info(coin).is_erc_20)
            {
                result.tx_hash = "0x" + result.tx_hash;
            }
        }
        return result;
    }

    void
    mm2::process_balance(const std::string& ticker) const
    {
        if (is_pin_cfg_enabled())
        {
            if (m_balance_informations.find(ticker) != m_balance_informations.end())
            {
                return;
            }
        }

        t_balance_request balance_request{.coin = ticker};
        auto              answer = rpc_balance(std::move(balance_request));
        if (answer.raw_result.find("error") == std::string::npos)
        {
            t_float_50 result = t_float_50(answer.balance) * m_balance_factor;
            answer.balance    = result.str();
            m_balance_informations.insert_or_assign(ticker, answer);
            this->dispatcher_.trigger<ticker_balance_updated>(ticker);
        }
    }

    void
    mm2::process_swaps()
    {
        std::size_t               total = this->m_swaps_registry.at("result").total;
        t_my_recent_swaps_request request{.limit = total > 0 ? total : 50};
        auto                      answer = rpc_my_recent_swaps(std::move(request));
        if (answer.result.has_value())
        {
            m_swaps_registry.insert_or_assign("result", answer.result.value());
            this->dispatcher_.trigger<process_swaps_finished>();
        }
    }

    void
    mm2::process_orders()
    {
        m_orders_registry.insert_or_assign("result", ::mm2::api::rpc_my_orders());
        this->dispatcher_.trigger<process_orders_finished>();
    }

    void
    mm2::process_tx(const std::string& ticker, bool is_a_refresh)
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        spdlog::trace("process_tx ticker: {}", ticker);
        ::mm2::api::tx_history_answer answer;
        if (not get_coin_info(ticker).is_erc_20)
        {
            t_tx_history_request tx_request{.coin = ticker, .limit = g_tx_max_limit};
            answer = rpc_my_tx_history(std::move(tx_request));
        }
        else
        {
            if (is_a_refresh)
            {
                return;
            }
            std::error_code ec;
            using namespace std::string_literals;
            std::string url = (ticker == "ETH") ? "https://komodo.live:3334/api/v1/eth_tx_history/"s + address(ticker, ec)
                                                : "https://komodo.live:3334/api/v1/erc_tx_history/"s + ticker + "/" + address(ticker, ec);
            answer          = ::mm2::api::process_rpc_get<::mm2::api::tx_history_answer>("tx_history", url);
        }


        if (answer.error.has_value())
        {
            spdlog::error("{}", answer.error.value());
        }
        else if (answer.rpc_result_code not_eq -1 and answer.result.has_value())
        {
            t_tx_state state;
            if (not get_coin_info(ticker).is_erc_20)
            {
                state.state             = answer.result.value().sync_status.state;
                state.current_block     = answer.result.value().current_block;
                state.blocks_left       = 0;
                state.transactions_left = 0;
            }
            else
            {
                state.state             = "Finished";
                state.current_block     = 0;
                state.blocks_left       = 0;
                state.transactions_left = 0;
            }

            if (answer.result.value().sync_status.additional_info.has_value())
            {
                if (answer.result.value().sync_status.additional_info.value().erc_infos.has_value())
                {
                    state.blocks_left = answer.result.value().sync_status.additional_info.value().erc_infos.value().blocks_left;
                }
                if (answer.result.value().sync_status.additional_info.value().regular_infos.has_value())
                {
                    state.transactions_left = answer.result.value().sync_status.additional_info.value().regular_infos.value().transactions_left;
                }
            }

            t_transactions out;
            out.reserve(answer.result.value().transactions.size());

            for (auto&& current: answer.result.value().transactions)
            {
                //spdlog::trace("my_balance change: {} ticker: {}", current.my_balance_change, ticker);

                tx_infos current_info{

                    .am_i_sender       = current.my_balance_change[0] == '-',
                    .confirmations     = current.confirmations.has_value() ? current.confirmations.value() : 0,
                    .from              = current.from,
                    .to                = current.to,
                    .date              = current.timestamp_as_date,
                    .timestamp         = current.timestamp,
                    .tx_hash           = current.tx_hash,
                    .fees              = current.fee_details.normal_fees.has_value() ? current.fee_details.normal_fees.value().amount
                                                                                     : current.fee_details.erc_fees.value().total_fee,
                    .my_balance_change = current.my_balance_change,
                    .total_amount      = current.total_amount,
                    .block_height      = current.block_height,
                    .ec                = dextop_error::success,
                };

                out.push_back(std::move(current_info));
            }

            std::sort(begin(out), end(out), [](auto&& a, auto&& b) { return a.timestamp > b.timestamp; });

            m_tx_informations.insert_or_assign(ticker, std::move(out));
            m_tx_state.insert_or_assign(ticker, std::move(state));
            this->dispatcher_.trigger<tx_fetch_finished>();
        }
    }

    void
    mm2::on_refresh_orderbook(const orderbook_refresh& evt)
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        spdlog::info("refreshing orderbook pair: [{} / {}]", evt.base, evt.rel);
        this->m_synchronized_ticker_pair = std::make_pair(evt.base, evt.rel);

        if (this->m_mm2_running)
        {
            spawn([this]() {
                batch_process_fees_and_fetch_current_orderbook_thread(true);
                // process_fees();
                // fetch_current_orderbook_thread(true);
            });
        }
    }

    void
    mm2::on_gui_enter_trading([[maybe_unused]] const gui_enter_trading& evt) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        m_orderbook_thread_active = true;
    }

    void
    mm2::on_gui_leave_trading([[maybe_unused]] const gui_leave_trading& evt) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        m_orderbook_thread_active = false;
    }

    t_buy_answer
    mm2::place_buy_order(t_buy_request&& request, const t_float_50& total, t_mm2_ec& ec) const
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        t_mm2_ec balance_ec;

        if (not do_i_have_enough_funds(request.rel, total))
        {
            ec = dextop_error::balance_not_enough_found;
            return {};
        }

        auto answer = ::mm2::api::rpc_buy(std::move(request));

        if (answer.error.has_value())
        {
            ec = dextop_error::rpc_buy_error;
            return {};
        }

        return answer;
    }

    bool
    mm2::do_i_have_enough_funds(const std::string& ticker, const t_float_50& amount) const
    {
        t_float_50 funds = get_balance(ticker);
        return funds >= amount;
    }

    std::string
    mm2::address(const std::string& ticker, t_mm2_ec& ec) const noexcept
    {
        if (m_balance_informations.find(ticker) == m_balance_informations.cend())
        {
            ec = dextop_error::unknown_ticker;
            return "Invalid";
        }

        return m_balance_informations.at(ticker).address;
    }

    ::mm2::api::my_orders_answer
    mm2::get_raw_orders(t_mm2_ec& ec) const noexcept
    {
        if (m_orders_registry.find("result") == m_orders_registry.cend())
        {
            ec = dextop_error::order_not_available_yet;
            return {};
        }
        return m_orders_registry.at("result");
    }

    ::mm2::api::my_orders_answer
    mm2::get_orders(const std::string& ticker, t_mm2_ec& ec) const noexcept
    {
        if (m_orders_registry.find("result") == m_orders_registry.cend())
        {
            ec = dextop_error::order_not_available_yet;
            return {};
        }
        auto  result                = m_orders_registry.at("result");
        auto& taker                 = result.taker_orders;
        auto& maker                 = result.maker_orders;
        auto  is_ticker_not_present = [&ticker](const std::pair<std::size_t, t_my_order_contents>& contents) {
            return contents.second.base != ticker && contents.second.rel != ticker;
        };

        erase_if(taker, is_ticker_not_present);
        erase_if(maker, is_ticker_not_present);

        return result;
    }

    std::vector<::mm2::api::my_orders_answer>
    mm2::get_orders(t_mm2_ec& ec) const noexcept
    {
        auto                                      coins = get_enabled_coins();
        std::vector<::mm2::api::my_orders_answer> out;
        out.reserve(coins.size());
        for (auto&& coin: coins) { out.emplace_back(get_orders(coin.ticker, ec)); }
        return out;
    }

    t_my_recent_swaps_answer
    mm2::get_swaps() const noexcept
    {
        return m_swaps_registry.at("result");
    }

    t_my_recent_swaps_answer
    mm2::get_swaps() noexcept
    {
        return m_swaps_registry.at("result");
    }

    t_sell_answer
    mm2::place_sell_order(t_sell_request&& request, const t_float_50& total, t_mm2_ec& ec) const
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        t_mm2_ec balance_ec;

        if (not do_i_have_enough_funds(request.base, total))
        {
            ec = dextop_error::balance_not_enough_found;
            return {.error = ec.message()};
        }

        auto answer = ::mm2::api::rpc_sell(std::move(request));

        if (answer.error.has_value())
        {
            ec = dextop_error::rpc_sell_error;
            return answer;
        }

        return answer;
    }

    t_tx_state
    mm2::get_tx_state(const std::string& ticker, t_mm2_ec& ec) const
    {
        if (m_tx_state.find(ticker) == m_tx_state.cend())
        {
            ec = dextop_error::tx_history_of_a_non_enabled_coin;
            return {};
        }

        return m_tx_state.at(ticker);
    }

    nlohmann::json
    mm2::claim_rewards(const std::string& ticker, t_mm2_ec& ec) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        nlohmann::json out  = nlohmann::json::object();
        const auto&    info = get_coin_info(ticker);
        if (not info.is_claimable)
        {
            ec = dextop_error::ticker_is_not_claimable;
            return {};
        }
        t_withdraw_request req{.coin = ticker, .to = m_balance_informations.at(ticker).address, .amount = "0", .max = true};
        auto               answer = ::mm2::api::rpc_withdraw(std::move(req));
        if (answer.rpc_result_code == 200)
        {
            out["withdraw_answer"]            = nlohmann::json::parse(answer.raw_result);
            out.at("withdraw_answer")["date"] = answer.result.value().timestamp_as_date;
            out["kmd_rewards_info"]           = ::mm2::api::rpc_kmd_rewards_info().result;
        }
        return out;
    }

    t_broadcast_answer
    mm2::send_rewards(t_broadcast_request&& req, t_mm2_ec& ec) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        auto ticker   = req.coin;
        auto b_answer = mm2::broadcast(std::move(req), ec);
        return b_answer;
    }

    t_float_50
    mm2::get_trade_fee(const std::string& ticker, const std::string& amount, bool is_max) const
    {
        t_float_50 sell_amount_f(amount);
        if (is_max)
        {
            std::error_code ec;
            sell_amount_f = t_float_50(my_balance(ticker, ec));
        }

        return t_float_50(1) / t_float_50(777) * sell_amount_f;
    }

    std::string
    mm2::get_trade_fee_str(const std::string& ticker, const std::string& sell_amount, bool is_max) const
    {
        std::stringstream ss;
        ss.precision(8);
        ss << std::fixed << get_trade_fee(ticker, sell_amount, is_max);
        return ss.str();
    }

    void
    mm2::apply_erc_fees(const std::string& ticker, t_float_50& value)
    {
        if (get_coin_info(ticker).is_erc_20)
        {
            spdlog::info("Calculating erc fees of rel ticker: {}", ticker);
            t_get_trade_fee_request rec_req{.coin = ticker};
            auto                    amount = get_trade_fixed_fee(ticker).amount;
            if (!amount.empty())
            {
                t_float_50 rec_amount = t_float_50(amount);
                value += rec_amount;
            }
        }
    }

    t_get_trade_fee_answer
    mm2::get_trade_fixed_fee(const std::string& ticker) const
    {
        return m_trade_fees_registry.find(ticker) != m_trade_fees_registry.cend() ? m_trade_fees_registry.at(ticker) : t_get_trade_fee_answer{};
    }

    bool
    mm2::is_orderbook_thread_active() const noexcept
    {
        return this->m_orderbook_thread_active.load();
    }

    nlohmann::json
    mm2::get_raw_mm2_ticker_cfg(const std::string& ticker) const noexcept
    {
        nlohmann::json out;
        if (m_mm2_raw_coins_cfg.find(ticker) != m_mm2_raw_coins_cfg.end())
        {
            atomic_dex::coin_element element = m_mm2_raw_coins_cfg.at(ticker);
            to_json(out, element);
            return out;
        }
        return nlohmann::json::object();
    }

    mm2::t_pair_max_vol
    mm2::get_taker_vol() const noexcept
    {
        return m_synchronized_max_taker_vol.value();
    }

    bool
    mm2::is_pin_cfg_enabled() const noexcept
    {
        return m_balance_factor != 1.0;
    }

    void
    mm2::reset_fake_balance_to_zero(const std::string& ticker) noexcept
    {
        auto answer    = m_balance_informations.at(ticker);
        answer.balance = "0";
        m_balance_informations.assign(ticker, answer);
        this->dispatcher_.trigger<ticker_balance_updated>(ticker);
    }

    void
    mm2::decrease_fake_balance(const std::string& ticker, const std::string& amount) noexcept
    {
        auto       answer = m_balance_informations.at(ticker);
        t_float_50 balance(answer.balance);
        t_float_50 amount_f(amount);
        t_float_50 result = balance - amount_f;
        spdlog::trace("decreasing {} - {} = {}", balance.str(8, std::ios_base::fixed), amount_f.str(8, std::ios_base::fixed), result.str(8, std::ios_base::fixed));
        if (result < 0)
        {
            reset_fake_balance_to_zero(ticker);
        }
        else
        {
            answer.balance = result.str(8, std::ios_base::fixed);
            m_balance_informations.assign(ticker, answer);
            this->dispatcher_.trigger<ticker_balance_updated>(ticker);
        }
    }
} // namespace atomic_dex
