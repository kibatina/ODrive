
#include "odrive_main.h"


Encoder::Encoder(const EncoderHardwareConfig_t& hw_config,
                Config_t& config) :
        hw_config_(hw_config),
        config_(config)
        //shaun. 构造函数在定义里面赋值. hw_config_这个指针指向hw_config, config_这个指针指向config.
{
    update_pll_gains();

    if (config.pre_calibrated && (config.mode == Encoder::MODE_HALL || config.mode == Encoder::MODE_SINCOS)) {
        is_ready_ = true;
    }
}

static void enc_index_cb_wrapper(void* ctx) {
    reinterpret_cast<Encoder*>(ctx)->enc_index_cb();
}

void Encoder::setup() {
    HAL_TIM_Encoder_Start(hw_config_.timer, TIM_CHANNEL_ALL);
    set_idx_subscribe();
}

void Encoder::set_error(Error_t error) {
    error_ |= error;
    axis_->error_ |= Axis::ERROR_ENCODER_FAILED;
}

bool Encoder::do_checks(){
    return error_ == ERROR_NONE;
}

//--------------------
// Hardware Dependent
//--------------------

// Triggered when an encoder passes over the "Index" pin
// TODO: only arm index edge interrupt when we know encoder has powered up
// (maybe by attaching the interrupt on start search, synergistic with following)
void Encoder::enc_index_cb() {
    if (config_.use_index) {
        set_circular_count(0, false);
        if (config_.zero_count_on_find_idx)
            set_linear_count(0); // Avoid position control transient after search
        if (config_.pre_calibrated) {
            is_ready_ = true;
        } else {
            // We can't use the update_offset facility in set_circular_count because
            // we also set the linear count before there is a chance to update. Therefore:
            // Invalidate offset calibration that may have happened before idx search
            is_ready_ = false;
        }
        index_found_ = true;
    }

    // Disable interrupt
    GPIO_unsubscribe(hw_config_.index_port, hw_config_.index_pin);
}

void Encoder::set_idx_subscribe(bool override_enable) {
    if (config_.use_index && (override_enable || !config_.find_idx_on_lockin_only)) {
        GPIO_subscribe(hw_config_.index_port, hw_config_.index_pin, GPIO_PULLDOWN,
                enc_index_cb_wrapper, this);
    } else if (!config_.use_index || config_.find_idx_on_lockin_only) {
        GPIO_unsubscribe(hw_config_.index_port, hw_config_.index_pin);
    }
}
	
void Encoder::update_pll_gains() {
		//shaun: bandwidth=1000,那么pll_kp_=2000,pll_ki_=1000000
    pll_kp_ = 2.0f * config_.bandwidth;  // basic conversion to discrete time
    pll_ki_ = 0.25f * (pll_kp_ * pll_kp_); // Critically damped
		//shaun, pll估算的参数。
    // Check that we don't get problems with discrete time approximation
    if (!(current_meas_period * pll_kp_ < 1.0f)) {
        set_error(ERROR_UNSTABLE_GAIN);
    }
}

void Encoder::check_pre_calibrated() {
    if (!is_ready_)
        config_.pre_calibrated = false;
    if (config_.mode == MODE_INCREMENTAL && !index_found_)
        config_.pre_calibrated = false;
}

// Function that sets the current encoder count to a desired 32-bit value.
void Encoder::set_linear_count(int32_t count) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    // Update states
    shadow_count_ = count;
    pos_estimate_ = (float)count;
    tim_cnt_sample_ = count;

    //Write hardware last
    hw_config_.timer->Instance->CNT = count;

    cpu_exit_critical(prim);
}

// Function that sets the CPR circular tracking encoder count to a desired 32-bit value.
// Note that this will get mod'ed down to [0, cpr)
void Encoder::set_circular_count(int32_t count, bool update_offset) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    if (update_offset) {
        config_.offset += count - count_in_cpr_;
        config_.offset = mod(config_.offset, config_.cpr);
    }

    // Update states
    count_in_cpr_ = mod(count, config_.cpr);
    pos_cpr_ = (float)count_in_cpr_;

    cpu_exit_critical(prim);
}

bool Encoder::run_index_search() {
    config_.use_index = true;
    index_found_ = false;
    if (!config_.idx_search_unidirectional && axis_->motor_.config_.direction == 0) {
        axis_->motor_.config_.direction = 1;
    }
    set_idx_subscribe();

    bool status = axis_->run_lockin_spin(axis_->config_.calibration_lockin);
    return status;
}

bool Encoder::run_direction_find() {
    int32_t init_enc_val = shadow_count_;
    bool orig_finish_on_distance = axis_->config_.calibration_lockin.finish_on_distance;
    axis_->config_.calibration_lockin.finish_on_distance = true;
    axis_->motor_.config_.direction = 1; // Must test spin forwards for direction detect logic
    bool status = axis_->run_lockin_spin(axis_->config_.calibration_lockin);
    axis_->config_.calibration_lockin.finish_on_distance = orig_finish_on_distance;

    if (status) {
        // Check response and direction
        if (shadow_count_ > init_enc_val + 8) {
            // motor same dir as encoder
            axis_->motor_.config_.direction = 1;
        } else if (shadow_count_ < init_enc_val - 8) {
            // motor opposite dir as encoder
            axis_->motor_.config_.direction = -1;
        } else {
            axis_->motor_.config_.direction = 0;
        }
    }

    return status;
}

// @brief Turns the motor in one direction for a bit and then in the other
// direction in order to find the offset between the electrical phase 0
// and the encoder state 0.
// TODO: Do the scan with current, not voltage!
bool Encoder::run_offset_calibration() {
    static const float start_lock_duration = 1.0f;
    static const int num_steps = (int)(config_.calib_scan_distance / config_.calib_scan_omega * (float)current_meas_hz);

    // Require index found if enabled
    if (config_.use_index && !index_found_) {
        set_error(ERROR_INDEX_NOT_FOUND_YET);
        return false;
    }

    // We use shadow_count_ to do the calibration, but the offset is used by count_in_cpr_
    // Therefore we have to sync them for calibration
    shadow_count_ = count_in_cpr_;

    float voltage_magnitude;
    if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_HIGH_CURRENT)
        voltage_magnitude = axis_->motor_.config_.calibration_current * axis_->motor_.config_.phase_resistance;
    else if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_GIMBAL)
        voltage_magnitude = axis_->motor_.config_.calibration_current;
    else
        return false;

    // go to motor zero phase for start_lock_duration to get ready to scan
    int i = 0;
    axis_->run_control_loop([&](){
        if (!axis_->motor_.enqueue_voltage_timings(voltage_magnitude, 0.0f))
            return false; // error set inside enqueue_voltage_timings
        axis_->motor_.log_timing(Motor::TIMING_LOG_ENC_CALIB);
        return ++i < start_lock_duration * current_meas_hz;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    int32_t init_enc_val = shadow_count_;
    int64_t encvaluesum = 0;

    // scan forward
    i = 0;
    axis_->run_control_loop([&](){
        float phase = wrap_pm_pi(config_.calib_scan_distance * (float)i / (float)num_steps - config_.calib_scan_distance / 2.0f);
        float v_alpha = voltage_magnitude * our_arm_cos_f32(phase);
        float v_beta = voltage_magnitude * our_arm_sin_f32(phase);
        if (!axis_->motor_.enqueue_voltage_timings(v_alpha, v_beta))
            return false; // error set inside enqueue_voltage_timings
        axis_->motor_.log_timing(Motor::TIMING_LOG_ENC_CALIB);

        encvaluesum += shadow_count_;
        
        return ++i < num_steps;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    // Check response and direction
    if (shadow_count_ > init_enc_val + 8) {
        // motor same dir as encoder
        axis_->motor_.config_.direction = 1;
    } else if (shadow_count_ < init_enc_val - 8) {
        // motor opposite dir as encoder
        axis_->motor_.config_.direction = -1;
    } else {
        // Encoder response error
        set_error(ERROR_NO_RESPONSE);
        return false;
    }

    //TODO avoid recomputing elec_rad_per_enc every time
    // Check CPR
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float expected_encoder_delta = config_.calib_scan_distance / elec_rad_per_enc;
    calib_scan_response_ = fabsf(shadow_count_-init_enc_val);
    if(fabsf(calib_scan_response_ - expected_encoder_delta)/expected_encoder_delta > config_.calib_range)
    {
        set_error(ERROR_CPR_OUT_OF_RANGE);
        return false;
    }

    // scan backwards
    i = 0;
    axis_->run_control_loop([&](){
        float phase = wrap_pm_pi(-config_.calib_scan_distance * (float)i / (float)num_steps + config_.calib_scan_distance / 2.0f);
        float v_alpha = voltage_magnitude * our_arm_cos_f32(phase);
        float v_beta = voltage_magnitude * our_arm_sin_f32(phase);
        if (!axis_->motor_.enqueue_voltage_timings(v_alpha, v_beta))
            return false; // error set inside enqueue_voltage_timings
        axis_->motor_.log_timing(Motor::TIMING_LOG_ENC_CALIB);

        encvaluesum += shadow_count_;
        
        return ++i < num_steps;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    config_.offset = encvaluesum / (num_steps * 2);
    int32_t residual = encvaluesum - ((int64_t)config_.offset * (int64_t)(num_steps * 2));
    config_.offset_float = (float)residual / (float)(num_steps * 2) + 0.5f; // add 0.5 to center-align state to phase

    is_ready_ = true;
    return true;
}

static bool decode_hall(uint8_t hall_state, int32_t* hall_cnt) {
    switch (hall_state) {
        case 0b001: *hall_cnt = 0; return true;
        case 0b011: *hall_cnt = 1; return true;
        case 0b010: *hall_cnt = 2; return true;
        case 0b110: *hall_cnt = 3; return true;
        case 0b100: *hall_cnt = 4; return true;
        case 0b101: *hall_cnt = 5; return true;
        default: return false;
    }
}

void Encoder::sample_now() {
    switch (config_.mode) {
        case MODE_INCREMENTAL: {
            tim_cnt_sample_ = (int16_t)hw_config_.timer->Instance->CNT;
        } break;

        case MODE_HALL: {
            // do nothing: samples already captured in general GPIO capture
        } break;

        case MODE_SINCOS: {
            sincos_sample_s_ = (get_adc_voltage(GPIO_3_GPIO_Port, GPIO_3_Pin) / 3.3f) - 0.5f;
            sincos_sample_c_ = (get_adc_voltage(GPIO_4_GPIO_Port, GPIO_4_Pin) / 3.3f) - 0.5f;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
        } break;
    }
}

bool Encoder::update() {
    // update internal encoder state.
    int32_t delta_enc = 0;
    switch (config_.mode) {
        case MODE_INCREMENTAL: {
            //TODO: use count_in_cpr_ instead as shadow_count_ can overflow
            //or use 64 bit
            //shaun: shadow_count_应该是历史数据.
            //shaun: tim_cnt_sample_是直接读取TIM的count.
            //shaun: 作为encoder的TIM并没有CPR=8196而把TIM的period设置为8195.而是固定0xFFFF，满量程。
            //shaun: 这里挺有意思的.shadow_count_通过int16变为int32,就可以保留历史信息,不会因为int16过界而回到0.
            //shaun: 也就是说如果这个tim是个32bit的timer,那么其count就已知是position,不需要任何的转变.
            //shaun: 前提是对tim的count满量程使用,不能16bit的用12bit,也不能32bit的用20bit之类的.
            int16_t delta_enc_16 = (int16_t)tim_cnt_sample_ - (int16_t)shadow_count_;
            delta_enc = (int32_t)delta_enc_16; //sign extend
        } break;

        case MODE_HALL: {
            int32_t hall_cnt;
            if (decode_hall(hall_state_, &hall_cnt)) {
                delta_enc = hall_cnt - count_in_cpr_;
                delta_enc = mod(delta_enc, 6);
                if (delta_enc > 3)
                    delta_enc -= 6;
            } else {
                if (!config_.ignore_illegal_hall_state) {
                    set_error(ERROR_ILLEGAL_HALL_STATE);
                    return false;
                }
            }
        } break;

        case MODE_SINCOS: {
            float phase = fast_atan2(sincos_sample_s_, sincos_sample_c_);
            int fake_count = (int)(1000.0f * phase);
            //CPR = 6283 = 2pi * 1k

            delta_enc = fake_count - count_in_cpr_;
            delta_enc = mod(delta_enc, 6283);
            if (delta_enc > 6283/2)
                delta_enc -= 6283;
        } break;
        
        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
           return false;
        } break;
    }

    shadow_count_ += delta_enc;
    count_in_cpr_ += delta_enc;
    count_in_cpr_ = mod(count_in_cpr_, config_.cpr);
		//shaun: count_in_cpr_ 倒是直接,明确在cpr范围内.
		//shaun: shadow_count_还没理解.如果是历史数据,为啥不叫last
    //// run pll (for now pll is in units of encoder counts)
    // Predict current pos
    pos_estimate_ += current_meas_period * vel_estimate_;
    pos_cpr_      += current_meas_period * vel_estimate_;
    // discrete phase detector
    //shaun: 把shadow_count_当成encoder产生的position.
    float delta_pos     = (float)(shadow_count_ - (int32_t)floorf(pos_estimate_));
    float delta_pos_cpr = (float)(count_in_cpr_ - (int32_t)floorf(pos_cpr_));
    delta_pos_cpr = wrap_pm(delta_pos_cpr, 0.5f * (float)(config_.cpr));
    // pll feedback
    //shaun: pos_estimate_是最终使用的位置信息.
    pos_estimate_ += current_meas_period * pll_kp_ * delta_pos;
    pos_cpr_      += current_meas_period * pll_kp_ * delta_pos_cpr;
    pos_cpr_ = fmodf_pos(pos_cpr_, (float)(config_.cpr));
		//shaun： vel_estimate_是最终使用的速度信息.
    vel_estimate_      += current_meas_period * pll_ki_ * delta_pos_cpr;
    bool snap_to_zero_vel = false;
		//shaun: TODO,没看懂.
		//shaun: current_meas_period=0.000125, pll_ki_=1000000. 0.5f * current_meas_period * pll_ki_= 62.5.
		//shaun: 配置的cpr=8196,那么62.5count/s意味着 (62.5/8196)*60=0.475RPM。
    if (fabsf(vel_estimate_) < 0.5f * current_meas_period * pll_ki_) {
        vel_estimate_ = 0.0f; //align delta-sigma on zero to prevent jitter
        snap_to_zero_vel = true;
    }

    //// run encoder count interpolation
    int32_t corrected_enc = count_in_cpr_ - config_.offset;
    // if we are stopped, make sure we don't randomly drift
    if (snap_to_zero_vel || !config_.enable_phase_interpolation) {
        interpolation_ = 0.5f;
    // reset interpolation if encoder edge comes
    } else if (delta_enc > 0) {
        interpolation_ = 0.0f;
    } else if (delta_enc < 0) {
        interpolation_ = 1.0f;
    } else {
        // Interpolate (predict) between encoder counts using vel_estimate,
        interpolation_ += current_meas_period * vel_estimate_;
        // don't allow interpolation indicated position outside of [enc, enc+1)
        if (interpolation_ > 1.0f) interpolation_ = 1.0f;
        if (interpolation_ < 0.0f) interpolation_ = 0.0f;
    }
    float interpolated_enc = corrected_enc + interpolation_;

    //// compute electrical phase
    //TODO avoid recomputing elec_rad_per_enc every time
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float ph = elec_rad_per_enc * (interpolated_enc - config_.offset_float);
    // ph = fmodf(ph, 2*M_PI);
    //shaun: phase_是角度,还是电子角度.
    //shaun: 如果编码器分辨率已经14bit也就是16384了,那么角度做插值意义不大.
    //shaun: 对于低分辨率的编码器有意义.
    phase_ = wrap_pm_pi(ph);

    return true;
}
