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
#include "atomic.dex.provider.coinpaprika.hpp"
#include "atomic.dex.http.code.hpp"
#include "atomic.threadpool.hpp"

namespace
{
    //! Using namespace
    using namespace std::chrono_literals;
    using namespace atomic_dex;
    using namespace atomic_dex::coinpaprika::api;

    nlohmann::json
    fetch_fiat_rates()
    {
        nlohmann::json resp;
        auto           answer = RestClient::get("https://api.openrates.io/latest?base=USD");
        if (answer.code != 200)
        {
            spdlog::warn("unable to fetch last open rates");
            return resp;
        }
        resp = nlohmann::json::parse(answer.body);
        return resp;
    }

    template <typename TAnswer, typename TRequest, typename TFunctorRequest>
    void
    retry(TAnswer& answer, const TRequest& request, TFunctorRequest&& functor)
    {
        while (answer.rpc_result_code == e_http_code::too_many_requests)
        {
            spdlog::warn("too many requests, retrying");
            std::this_thread::sleep_for(1s);
            functor(request);
        }
    }

    void
    process_ticker_infos(const atomic_dex::coin_config& current_coin, atomic_dex::coinpaprika_provider::t_ticker_infos_registry& reg)
    {
        const ticker_infos_request request{.ticker_currency_id = current_coin.coinpaprika_id, .ticker_quotes = {"USD", "EUR", "BTC"}};
        auto                       answer = tickers_info(request);

        retry(answer, request, [&answer](const ticker_infos_request& request) { answer = tickers_info(request); });
        reg.insert_or_assign(current_coin.ticker, answer);
    }

    void
    process_ticker_historical(const atomic_dex::coin_config& current_coin, atomic_dex::coinpaprika_provider::t_ticker_historical_registry& reg)
    {
        if (current_coin.coinpaprika_id == "test-coin")
        {
            return;
        }
        const ticker_historical_request request{.ticker_currency_id = current_coin.coinpaprika_id, .interval = "2h"};
        auto                            answer = ticker_historical(request);
        retry(answer, request, [&answer](const ticker_historical_request& request) { answer = ticker_historical(request); });
        if (answer.raw_result.find("error") == std::string::npos)
        {
            reg.insert_or_assign(current_coin.ticker, answer);
        }
    }

    template <typename Provider>
    void
    process_provider(const atomic_dex::coin_config& current_coin, Provider& rate_providers, const std::string& fiat)
    {
        if (current_coin.coinpaprika_id != fiat)
        {
            const auto                    base = current_coin.coinpaprika_id;
            const price_converter_request request{.base_currency_id = base, .quote_currency_id = fiat};
            auto                          answer = price_converter(request);

            retry(answer, request, [&answer](const price_converter_request& request) { answer = price_converter(request); });

            if (answer.raw_result.find("error") == std::string::npos)
            {
                if (not answer.price.empty())
                {
                    rate_providers.insert_or_assign(current_coin.ticker, answer.price);
                }
            }
            else
                rate_providers.insert_or_assign(current_coin.ticker, "0.00");
        }
        else
        {
            rate_providers.insert_or_assign(current_coin.ticker, "1.00");
        }
    }

    std::string
    compute_result(const std::string& amount, const std::string& price, const std::string& currency, atomic_dex::cfg& cfg)
    {
        const t_float_50 amount_f(amount);
        const t_float_50 current_price_f(price);
        const t_float_50 final_price       = amount_f * current_price_f;
        std::size_t      default_precision = atomic_dex::is_this_currency_a_fiat(cfg, currency) ? 2 : 8;
        std::string      result;

        if (auto final_price_str = final_price.str(default_precision, std::ios_base::fixed); final_price_str == "0.00" && final_price > 0.00000000)
        {
            const auto retry = [&result, &final_price, &default_precision]() { result = final_price.str(default_precision, std::ios_base::fixed); };

            result = final_price.str(default_precision);
            if (result.find("e") != std::string::npos)
            {
                //! We have scientific notations lets get ride of that
                do {
                    default_precision += 1;
                    retry();
                } while (t_float_50(result) <= 0);
            }
        }
        else
        {
            result = final_price.str(default_precision, std::ios_base::fixed);
        }

        boost::trim_right_if(result, boost::is_any_of("0"));
        boost::trim_right_if(result, boost::is_any_of("."));
        return result;
    }
} // namespace

namespace atomic_dex
{
    namespace bm = boost::multiprecision;

    coinpaprika_provider::coinpaprika_provider(entt::registry& registry, mm2& mm2_instance, atomic_dex::cfg& cfg) :
        system(registry), m_mm2_instance(mm2_instance), m_cfg(cfg)
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        disable();
        dispatcher_.sink<mm2_started>().connect<&coinpaprika_provider::on_mm2_started>(*this);
        dispatcher_.sink<coin_enabled>().connect<&coinpaprika_provider::on_coin_enabled>(*this);
        dispatcher_.sink<coin_disabled>().connect<&coinpaprika_provider::on_coin_disabled>(*this);
    }

    void
    coinpaprika_provider::update() noexcept
    {
    }

    coinpaprika_provider::~coinpaprika_provider() noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        m_provider_thread_timer.interrupt();
        if (m_provider_rates_thread.joinable())
        {
            m_provider_rates_thread.join();
        }
        dispatcher_.sink<mm2_started>().disconnect<&coinpaprika_provider::on_mm2_started>(*this);
        dispatcher_.sink<coin_enabled>().disconnect<&coinpaprika_provider::on_coin_enabled>(*this);
        dispatcher_.sink<coin_disabled>().disconnect<&coinpaprika_provider::on_coin_disabled>(*this);
    }

    void
    coinpaprika_provider::on_mm2_started([[maybe_unused]] const mm2_started& evt) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        m_provider_rates_thread = std::thread([this]() {
            spdlog::info("paprika thread started");

            using namespace std::chrono_literals;
            do {
                spdlog::info("refreshing rate conversion from coinpaprika");

                t_coins coins = m_mm2_instance.get_enabled_coins();

                std::vector<std::future<void>> out_fut;

                out_fut.reserve(coins.size() * 6 + 1);
                out_fut.push_back(spawn([this]() { this->m_other_fiats_rates = fetch_fiat_rates(); }));
                for (auto&& current_coin: coins)
                {
                    if (current_coin.coinpaprika_id == "test-coin")
                    {
                        continue;
                    }
                    out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_ticker_infos(cur_coin, this->m_ticker_infos_registry); }));
                    out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_ticker_historical(cur_coin, this->m_ticker_historical_registry); }));
                    out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_provider(cur_coin, m_usd_rate_providers, "usd-us-dollars"); }));
                    if (current_coin.ticker != "BTC")
                    {
                        out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_provider(cur_coin, m_btc_rate_providers, "btc-bitcoin"); }));
                    }
                    if (current_coin.ticker != "KMD")
                    {
                        out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_provider(cur_coin, m_kmd_rate_providers, "kmd-komodo"); }));
                    }
                    out_fut.push_back(spawn([this, cur_coin = current_coin]() { process_provider(cur_coin, m_eur_rate_providers, "eur-euro"); }));
                }
                for (auto&& cur_fut: out_fut) { cur_fut.get(); }
            } while (not m_provider_thread_timer.wait_for(120s));
        });
    }

    std::string
    coinpaprika_provider::get_price_in_fiat(const std::string& fiat, const std::string& ticker, std::error_code& ec, bool skip_precision) const noexcept
    {
        if (m_supported_fiat_registry.count(fiat) == 0u)
        {
            ec = dextop_error::invalid_fiat_for_rate_conversion;
            return "0.00";
        }

        if (m_mm2_instance.get_coin_info(ticker).coinpaprika_id == "test-coin")
        {
            return "0.00";
        }

        const auto price = get_rate_conversion(fiat, ticker, ec);

        if (ec)
        {
            return "0.00";
        }

        std::error_code t_ec;
        const auto      amount = m_mm2_instance.my_balance(ticker, t_ec);

        if (t_ec)
        {
            ec = t_ec;
            spdlog::error("my_balance error: {}", t_ec.message());
            return "0.00";
        }

        if (not skip_precision)
        {
            return compute_result(amount, price, fiat, this->m_cfg);
        }

        const t_float_50  price_f(price);
        const t_float_50  amount_f(amount);
        const t_float_50  final_price = price_f * amount_f;
        std::stringstream ss;

        ss << std::fixed << final_price;

        return ss.str() == "0" ? "0.00" : ss.str();
    }

    std::string
    coinpaprika_provider::get_price_in_fiat_all(const std::string& fiat, std::error_code& ec) const noexcept
    {
        t_coins coins = m_mm2_instance.get_enabled_coins();
        try
        {
            t_float_50        final_price_f = 0;
            std::string       current_price = "0.00";
            std::stringstream ss;

            for (auto&& current_coin: coins)
            {
                if (current_coin.coinpaprika_id == "test-coin")
                {
                    continue;
                }

                current_price = get_price_in_fiat(fiat, current_coin.ticker, ec, true);

                if (ec)
                {
                    spdlog::warn("error when converting {} to {}, err: {}", current_coin.ticker, fiat, ec.message());
                    ec.clear(); //! Reset
                    continue;
                }

                if (not current_price.empty())
                {
                    const auto current_price_f = t_float_50(current_price);
                    final_price_f += current_price_f;
                }
            }

            std::size_t default_precision = is_this_currency_a_fiat(m_cfg, fiat) ? 2 : 8;
            ss.precision(default_precision);
            ss << std::fixed << final_price_f;
            std::string result = ss.str();
            boost::trim_right_if(result, boost::is_any_of("0"));
            boost::trim_right_if(result, boost::is_any_of("."));
            return result;
        }
        catch (const std::exception& error)
        {
            spdlog::error("exception caught: {}", error.what());
            return "0.00";
        }
    }

    std::string
    coinpaprika_provider::get_price_as_currency_from_tx(
        const std::string& currency, const std::string& ticker, const tx_infos& tx, std::error_code& ec) const noexcept
    {
        if (m_mm2_instance.get_coin_info(ticker).coinpaprika_id == "test-coin")
        {
            return "0.00";
        }
        const auto amount        = tx.am_i_sender ? tx.my_balance_change.substr(1) : tx.my_balance_change;
        const auto current_price = get_rate_conversion(currency, ticker, ec);
        if (ec)
        {
            return "0.00";
        }
        return compute_result(amount, current_price, currency, this->m_cfg);
    }

    std::string
    coinpaprika_provider::get_price_as_currency_from_amount(
        const std::string& currency, const std::string& ticker, const std::string& amount, std::error_code& ec) const noexcept
    {
        if (m_mm2_instance.get_coin_info(ticker).coinpaprika_id == "test-coin")
        {
            return "0.00";
        }

        const auto current_price = get_rate_conversion(currency, ticker, ec);

        if (ec)
        {
            return "0.00";
        }

        return compute_result(amount, current_price, currency, this->m_cfg);
    }

    std::string
    coinpaprika_provider::get_rate_conversion(const std::string& fiat, const std::string& ticker, std::error_code& ec, bool adjusted) const noexcept
    {
        std::string current_price;

        if (fiat == "USD")
        {
            //! Do it as usd;
            if (m_usd_rate_providers.find(ticker) == m_usd_rate_providers.cend())
            {
                ec = dextop_error::unknown_ticker_for_rate_conversion;
                return "0.00";
            }
            current_price = m_usd_rate_providers.at(ticker);
        }
        else if (fiat == "EUR")
        {
            if (m_eur_rate_providers.find(ticker) == m_eur_rate_providers.cend())
            {
                ec = dextop_error::unknown_ticker_for_rate_conversion;
                return "0.00";
            }
            current_price = m_eur_rate_providers.at(ticker);
        }
        else if (fiat == "BTC")
        {
            if (ticker == "BTC")
            {
                return "1.00";
            }
            if (m_btc_rate_providers.find(ticker) == m_btc_rate_providers.cend())
            {
                ec = dextop_error::unknown_ticker_for_rate_conversion;
                return "0.00";
            }
            current_price = m_btc_rate_providers.at(ticker);
        }
        else if (fiat == "KMD")
        {
            if (ticker == "KMD")
            {
                return "1.00";
            }
            if (m_kmd_rate_providers.find(ticker) == m_kmd_rate_providers.cend())
            {
                ec = dextop_error::unknown_ticker_for_rate_conversion;
                return "0.00";
            }
            current_price = m_kmd_rate_providers.at(ticker);
        }
        else
        {
            if (m_usd_rate_providers.find(ticker) == m_usd_rate_providers.cend())
            {
                ec = dextop_error::unknown_ticker_for_rate_conversion;
                return "0.00";
            }
            t_float_50 tmp_current_price = t_float_50(m_usd_rate_providers.at(ticker)) * m_other_fiats_rates->at("rates").at(fiat).get<double>();
            current_price                = tmp_current_price.str();
        }

        if (adjusted)
        {
            std::size_t default_precision = is_this_currency_a_fiat(m_cfg, fiat) ? 2 : 8;

            t_float_50 current_price_f(current_price);
            if (is_this_currency_a_fiat(m_cfg, fiat))
            {
                if (current_price_f < 1.0)
                {
                    default_precision = 5;
                }
            }
            //! Trick: If there conversion in a fixed representation is 0.00 then use a default precision to 2 without fixed ios flags
            if (auto fixed_str = current_price_f.str(default_precision, std::ios_base::fixed); fixed_str == "0.00" && current_price_f > 0.00000000)
            {
                return current_price_f.str(default_precision);
            }
            return current_price_f.str(default_precision, std::ios::fixed);
        }
        return current_price;
    }

    void
    coinpaprika_provider::on_coin_enabled(const coin_enabled& evt) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        if (not this->m_other_fiats_rates->contains("rates"))
        {
            this->m_other_fiats_rates = fetch_fiat_rates();
        }
        const auto config = m_mm2_instance.get_coin_info(evt.ticker);

        if (config.coinpaprika_id != "test-coin")
        {
            spawn([config, evt, this]() {
                process_provider(config, m_usd_rate_providers, "usd-us-dollars");
                process_provider(config, m_eur_rate_providers, "eur-euro");
                if (evt.ticker != "BTC")
                {
                    process_provider(config, m_btc_rate_providers, "btc-bitcoin");
                }
                if (evt.ticker != "KMD")
                {
                    process_provider(config, m_kmd_rate_providers, "kmd-komodo");
                }
                process_ticker_infos(config, m_ticker_infos_registry);
                process_ticker_historical(config, m_ticker_historical_registry);
                this->dispatcher_.trigger<coin_fully_initialized>(evt.ticker);
            });
        }
        else
        {
            this->dispatcher_.trigger<coin_fully_initialized>(evt.ticker);
        }
    }

    void
    coinpaprika_provider::on_coin_disabled(const coin_disabled& evt) noexcept
    {
        spdlog::debug("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        const auto config = m_mm2_instance.get_coin_info(evt.ticker);

        m_usd_rate_providers.erase(config.ticker);
        m_eur_rate_providers.erase(config.ticker);
        if (evt.ticker != "BTC")
        {
            m_btc_rate_providers.erase(config.ticker);
        }
        if (evt.ticker != "KMD")
        {
            m_kmd_rate_providers.erase(config.ticker);
        }
    }

    t_ticker_info_answer
    coinpaprika_provider::get_ticker_infos(const std::string& ticker) const noexcept
    {
        return m_ticker_infos_registry.find(ticker) != m_ticker_infos_registry.cend() ? m_ticker_infos_registry.at(ticker) : t_ticker_info_answer{};
    }

    t_ticker_historical_answer
    coinpaprika_provider::get_ticker_historical(const std::string& ticker) const noexcept
    {
        return m_ticker_historical_registry.find(ticker) != m_ticker_historical_registry.cend() ? m_ticker_historical_registry.at(ticker)
                                                                                                : t_ticker_historical_answer{.answer = nlohmann::json::array()};
    }

    std::string
    coinpaprika_provider::get_cex_rates(const std::string& base, const std::string& rel, std::error_code& ec) const noexcept
    {
        std::string base_rate_str = get_rate_conversion("USD", base, ec, false);
        if (ec)
        {
            return "0.00";
        }
        std::string rel_rate_str = get_rate_conversion("USD", rel, ec, false);
        if (ec)
        {
            return "0.00";
        }
        if (base_rate_str == "0.00" || rel_rate_str == "0.00")
        {
            //! One of the rate is not available
            return "0.00";
        }
        t_float_50  base_rate_f(base_rate_str);
        t_float_50  rel_rate_f(rel_rate_str);
        t_float_50  result     = base_rate_f / rel_rate_f;
        std::string result_str = result.str(8, std::ios_base::fixed);
        boost::trim_right_if(result_str, boost::is_any_of("0"));
        boost::trim_right_if(result_str, boost::is_any_of("."));
        return result_str;
    }
} // namespace atomic_dex
