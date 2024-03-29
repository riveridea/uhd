//
// Copyright 2011-2012 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "db_wbx_common.hpp"
#include "adf4350_regs.hpp"
#include <uhd/utils/log.hpp>
#include <uhd/types/dict.hpp>
#include <uhd/types/ranges.hpp>
#include <uhd/types/sensors.hpp>
#include <uhd/utils/assert_has.hpp>
#include <uhd/utils/algorithm.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/usrp/dboard_base.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/format.hpp>
#include <boost/math/special_functions/round.hpp>

using namespace uhd;
using namespace uhd::usrp;
using namespace boost::assign;


/***********************************************************************
 * WBX Version 2 Constants
 **********************************************************************/
static const uhd::dict<std::string, gain_range_t> wbx_v2_tx_gain_ranges = map_list_of
    ("PGA0", gain_range_t(0, 25, 0.05))
;

static const freq_range_t wbx_v2_freq_range(68.75e6, 2.2e9);

/***********************************************************************
 * Gain-related functions
 **********************************************************************/
static double tx_pga0_gain_to_dac_volts(double &gain){
    //clip the input
    gain = wbx_v2_tx_gain_ranges["PGA0"].clip(gain);

    //voltage level constants
    static const double max_volts = 0.5, min_volts = 1.4;
    static const double slope = (max_volts-min_volts)/wbx_v2_tx_gain_ranges["PGA0"].stop();

    //calculate the voltage for the aux dac
    double dac_volts = gain*slope + min_volts;

    UHD_LOGV(often) << boost::format(
        "WBX TX Gain: %f dB, dac_volts: %f V"
    ) % gain % dac_volts << std::endl;

    //the actual gain setting
    gain = (dac_volts - min_volts)/slope;

    return dac_volts;
}


/***********************************************************************
 * WBX Version 2 Implementation
 **********************************************************************/
wbx_base::wbx_version2::wbx_version2(wbx_base *_self_wbx_base) {
    //register our handle on the primary wbx_base instance
    self_base = _self_wbx_base;

    ////////////////////////////////////////////////////////////////////
    // Register RX properties
    ////////////////////////////////////////////////////////////////////
    this->get_rx_subtree()->create<std::string>("name").set("WBXv2 RX");
    this->get_rx_subtree()->create<double>("freq/value")
         .coerce(boost::bind(&wbx_base::wbx_version2::set_lo_freq, this, dboard_iface::UNIT_RX, _1))
         .set((wbx_v2_freq_range.start() + wbx_v2_freq_range.stop())/2.0);
    this->get_rx_subtree()->create<meta_range_t>("freq/range").set(wbx_v2_freq_range);

    ////////////////////////////////////////////////////////////////////
    // Register TX properties
    ////////////////////////////////////////////////////////////////////
    this->get_tx_subtree()->create<std::string>("name").set("WBXv2 TX");
    BOOST_FOREACH(const std::string &name, wbx_v2_tx_gain_ranges.keys()){
        self_base->get_tx_subtree()->create<double>("gains/"+name+"/value")
            .coerce(boost::bind(&wbx_base::wbx_version2::set_tx_gain, this, _1, name))
            .set(wbx_v2_tx_gain_ranges[name].start());
        self_base->get_tx_subtree()->create<meta_range_t>("gains/"+name+"/range")
            .set(wbx_v2_tx_gain_ranges[name]);
    }
    this->get_tx_subtree()->create<double>("freq/value")
         .coerce(boost::bind(&wbx_base::wbx_version2::set_lo_freq, this, dboard_iface::UNIT_TX, _1))
         .set((wbx_v2_freq_range.start() + wbx_v2_freq_range.stop())/2.0);
    this->get_tx_subtree()->create<meta_range_t>("freq/range").set(wbx_v2_freq_range);
    this->get_tx_subtree()->create<bool>("enabled")
        .subscribe(boost::bind(&wbx_base::wbx_version2::set_tx_enabled, this, _1))
        .set(true); //start enabled

    //set attenuator control bits
    int v2_iobits = ADF4350_CE;
    int v2_tx_mod = TXMOD_EN|ADF4350_PDBRF;

    //set the gpio directions and atr controls
    self_base->get_iface()->set_pin_ctrl(dboard_iface::UNIT_TX, v2_tx_mod);
    self_base->get_iface()->set_pin_ctrl(dboard_iface::UNIT_RX, RXBB_PDB|ADF4350_PDBRF);
    self_base->get_iface()->set_gpio_ddr(dboard_iface::UNIT_TX, TX_PUP_5V|TX_PUP_3V|v2_tx_mod|v2_iobits);
    self_base->get_iface()->set_gpio_ddr(dboard_iface::UNIT_RX, RX_PUP_5V|RX_PUP_3V|ADF4350_CE|RXBB_PDB|ADF4350_PDBRF|RX_ATTN_MASK);

    //setup ATR for the mixer enables (always enabled to prevent phase slip between bursts)
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_IDLE,        v2_tx_mod, TX_MIXER_DIS | v2_tx_mod);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_RX_ONLY,     v2_tx_mod, TX_MIXER_DIS | v2_tx_mod);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_TX_ONLY,     v2_tx_mod, TX_MIXER_DIS | v2_tx_mod);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_FULL_DUPLEX, v2_tx_mod, TX_MIXER_DIS | v2_tx_mod);

    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_IDLE,        RX_MIXER_ENB, RX_MIXER_DIS | RX_MIXER_ENB);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_TX_ONLY,     RX_MIXER_ENB, RX_MIXER_DIS | RX_MIXER_ENB);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_RX_ONLY,     RX_MIXER_ENB, RX_MIXER_DIS | RX_MIXER_ENB);
    self_base->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_FULL_DUPLEX, RX_MIXER_ENB, RX_MIXER_DIS | RX_MIXER_ENB);
}

wbx_base::wbx_version2::~wbx_version2(void){
    /* NOP */
}

/***********************************************************************
 * Enables
 **********************************************************************/
void wbx_base::wbx_version2::set_tx_enabled(bool enb){
    self_base->get_iface()->set_gpio_out(dboard_iface::UNIT_TX,
        (enb)? TX_POWER_UP | ADF4350_CE : TX_POWER_DOWN, TX_POWER_UP | TX_POWER_DOWN | ADF4350_CE);
}


/***********************************************************************
 * Gain Handling
 **********************************************************************/
double wbx_base::wbx_version2::set_tx_gain(double gain, const std::string &name){
    assert_has(wbx_v2_tx_gain_ranges.keys(), name, "wbx tx gain name");
    if(name == "PGA0"){
        double dac_volts = tx_pga0_gain_to_dac_volts(gain);
        self_base->_tx_gains[name] = gain;

        //write the new voltage to the aux dac
        self_base->get_iface()->write_aux_dac(dboard_iface::UNIT_TX, dboard_iface::AUX_DAC_A, dac_volts);
    }
    else UHD_THROW_INVALID_CODE_PATH();
    return self_base->_tx_gains[name]; //shadowed
}


/***********************************************************************
 * Tuning
 **********************************************************************/
double wbx_base::wbx_version2::set_lo_freq(dboard_iface::unit_t unit, double target_freq) {
    //clip to tuning range
    target_freq = wbx_v2_freq_range.clip(target_freq);

    UHD_LOGV(often) << boost::format(
        "WBX tune: target frequency %f Mhz"
    ) % (target_freq/1e6) << std::endl;

    //start with target_freq*2 because mixer has divide by 2
    target_freq *= 2;

    //map prescaler setting to mininmum integer divider (N) values (pg.18 prescaler)
    static const uhd::dict<int, int> prescaler_to_min_int_div = map_list_of
        (0,23) //adf4350_regs_t::PRESCALER_4_5
        (1,75) //adf4350_regs_t::PRESCALER_8_9
    ;

    //map rf divider select output dividers to enums
    static const uhd::dict<int, adf4350_regs_t::rf_divider_select_t> rfdivsel_to_enum = map_list_of
        (1,  adf4350_regs_t::RF_DIVIDER_SELECT_DIV1)
        (2,  adf4350_regs_t::RF_DIVIDER_SELECT_DIV2)
        (4,  adf4350_regs_t::RF_DIVIDER_SELECT_DIV4)
        (8,  adf4350_regs_t::RF_DIVIDER_SELECT_DIV8)
        (16, adf4350_regs_t::RF_DIVIDER_SELECT_DIV16)
    ;

    double actual_freq, pfd_freq;
    double ref_freq = self_base->get_iface()->get_clock_rate(unit);
    int R=0, BS=0, N=0, FRAC=0, MOD=0;
    int RFdiv = 1;
    adf4350_regs_t::reference_divide_by_2_t T     = adf4350_regs_t::REFERENCE_DIVIDE_BY_2_DISABLED;
    adf4350_regs_t::reference_doubler_t     D     = adf4350_regs_t::REFERENCE_DOUBLER_DISABLED;    

    //Reference doubler for 50% duty cycle
    // if ref_freq < 12.5MHz enable regs.reference_divide_by_2
    if(ref_freq <= 12.5e6) D = adf4350_regs_t::REFERENCE_DOUBLER_ENABLED;

    //increase RF divider until acceptable VCO frequency
    double vco_freq = target_freq;
    while (vco_freq < 2.2e9) {
        vco_freq *= 2;
        RFdiv *= 2;
    }

    //use 8/9 prescaler for vco_freq > 3 GHz (pg.18 prescaler)
    adf4350_regs_t::prescaler_t prescaler = target_freq > 3e9 ? adf4350_regs_t::PRESCALER_8_9 : adf4350_regs_t::PRESCALER_4_5;

    /*
     * The goal here is to loop though possible R dividers,
     * band select clock dividers, N (int) dividers, and FRAC 
     * (frac) dividers.
     *
     * Calculate the N and F dividers for each set of values.
     * The loop exits when it meets all of the constraints.
     * The resulting loop values are loaded into the registers.
     *
     * from pg.21
     *
     * f_pfd = f_ref*(1+D)/(R*(1+T))
     * f_vco = (N + (FRAC/MOD))*f_pfd
     *    N = f_vco/f_pfd - FRAC/MOD = f_vco*((R*(T+1))/(f_ref*(1+D))) - FRAC/MOD
     * f_rf = f_vco/RFdiv)
     * f_actual = f_rf/2
     */
    for(R = 1; R <= 1023; R+=1){
        //PFD input frequency = f_ref/R ... ignoring Reference doubler/divide-by-2 (D & T)
        pfd_freq = ref_freq*(1+D)/(R*(1+T));

        //keep the PFD frequency at or below 25MHz (Loop Filter Bandwidth)
        if (pfd_freq > 25e6) continue;

        //ignore fractional part of tuning
        N = int(std::floor(target_freq/pfd_freq));

        //keep N > minimum int divider requirement
        if (N < prescaler_to_min_int_div[prescaler]) continue;

        for(BS=1; BS <= 255; BS+=1){
            //keep the band select frequency at or below 100KHz
            //constraint on band select clock
            if (pfd_freq/BS > 100e3) continue;
            goto done_loop;
        }
    } done_loop:

    //Fractional-N calculation
    MOD = 4095; //max fractional accuracy
    FRAC = int((target_freq/pfd_freq - N)*MOD);

    //Reference divide-by-2 for 50% duty cycle
    // if R even, move one divide by 2 to to regs.reference_divide_by_2
    if(R % 2 == 0){
        T = adf4350_regs_t::REFERENCE_DIVIDE_BY_2_ENABLED;
        R /= 2;
    }

    //actual frequency calculation
    actual_freq = double((N + (double(FRAC)/double(MOD)))*ref_freq*(1+int(D))/(R*(1+int(T)))/2);

    UHD_LOGV(often)
        << boost::format("WBX Intermediates: ref=%0.2f, outdiv=%f, fbdiv=%f") % (ref_freq*(1+int(D))/(R*(1+int(T)))) % double(RFdiv*2) % double(N + double(FRAC)/double(MOD)) << std::endl

        << boost::format("WBX tune: R=%d, BS=%d, N=%d, FRAC=%d, MOD=%d, T=%d, D=%d, RFdiv=%d"
            ) % R % BS % N % FRAC % MOD % T % D % RFdiv << std::endl
        << boost::format("WBX Frequencies (MHz): REQ=%0.2f, ACT=%0.2f, VCO=%0.2f, PFD=%0.2f, BAND=%0.2f"
            ) % (target_freq/1e6) % (actual_freq/1e6) % (vco_freq/1e6) % (pfd_freq/1e6) % (pfd_freq/BS/1e6) << std::endl;

    //load the register values
    adf4350_regs_t regs;

    regs.frac_12_bit = FRAC;
    regs.int_16_bit = N;
    regs.mod_12_bit = MOD;
    regs.clock_divider_12_bit = std::max(1, int(std::ceil(400e-6*pfd_freq/MOD)));
    regs.feedback_select = adf4350_regs_t::FEEDBACK_SELECT_DIVIDED;
    regs.clock_div_mode = adf4350_regs_t::CLOCK_DIV_MODE_RESYNC_ENABLE;
    regs.prescaler = prescaler;
    regs.r_counter_10_bit = R;
    regs.reference_divide_by_2 = T;
    regs.reference_doubler = D;
    regs.band_select_clock_div = BS;
    UHD_ASSERT_THROW(rfdivsel_to_enum.has_key(RFdiv));
    regs.rf_divider_select = rfdivsel_to_enum[RFdiv];

    if (unit == dboard_iface::UNIT_RX) {
        freq_range_t rx_lo_5dbm = list_of
            (range_t(0.05e9, 1.4e9))
        ;

        freq_range_t rx_lo_2dbm = list_of
            (range_t(1.4e9, 2.2e9))
        ;

        if (actual_freq == rx_lo_5dbm.clip(actual_freq)) regs.output_power = adf4350_regs_t::OUTPUT_POWER_5DBM;

        if (actual_freq == rx_lo_2dbm.clip(actual_freq)) regs.output_power = adf4350_regs_t::OUTPUT_POWER_2DBM;

    } else if (unit == dboard_iface::UNIT_TX) {
        freq_range_t tx_lo_5dbm = list_of
            (range_t(0.05e9, 1.7e9))
            (range_t(1.9e9, 2.2e9))
        ;

        freq_range_t tx_lo_m1dbm = list_of
            (range_t(1.7e9, 1.9e9))
        ;

        if (actual_freq == tx_lo_5dbm.clip(actual_freq)) regs.output_power = adf4350_regs_t::OUTPUT_POWER_5DBM;

        if (actual_freq == tx_lo_m1dbm.clip(actual_freq)) regs.output_power = adf4350_regs_t::OUTPUT_POWER_M1DBM;

    }

    //write the registers
    //correct power-up sequence to write registers (5, 4, 3, 2, 1, 0)
    int addr;

    for(addr=5; addr>=0; addr--){
        UHD_LOGV(often) << boost::format(
            "WBX SPI Reg (0x%02x): 0x%08x"
        ) % addr % regs.get_reg(addr) << std::endl;
        self_base->get_iface()->write_spi(
            unit, spi_config_t::EDGE_RISE,
            regs.get_reg(addr), 32
        );
    }

    //return the actual frequency
    UHD_LOGV(often) << boost::format(
        "WBX tune: actual frequency %f Mhz"
    ) % (actual_freq/1e6) << std::endl;
    return actual_freq;
}
