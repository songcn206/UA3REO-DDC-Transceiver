-- -------------------------------------------------------------
--
-- Module: tx_cic
-- Generated by MATLAB(R) 9.6 and Filter Design HDL Coder 3.1.5.
-- Generated on: 2019-11-09 17:11:10
-- -------------------------------------------------------------

-- -------------------------------------------------------------
-- HDL Code Generation Options:
--
-- TargetLanguage: VHDL
-- OptimizeForHDL: on
-- EDAScriptGeneration: off
-- AddPipelineRegisters: on
-- Name: tx_cic
-- TestBenchName: tx_cic_tb
-- TestBenchStimulus: step ramp chirp noise 
-- GenerateHDLTestBench: off

-- -------------------------------------------------------------
-- HDL Implementation    : Fully parallel
-- -------------------------------------------------------------
-- Filter Settings:
--
-- Discrete-Time FIR Multirate Filter (real)
-- -----------------------------------------
-- Filter Structure      : Cascaded Integrator-Comb Interpolator
-- Interpolation Factor  : 2560
-- Differential Delay    : 1
-- Number of Sections    : 2
-- Stable                : Yes
-- Linear Phase          : Yes (Type 1)
--
-- -------------------------------------------------------------



LIBRARY IEEE;
USE IEEE.std_logic_1164.all;
USE IEEE.numeric_std.ALL;

ENTITY tx_cic IS
   PORT( clk                             :   IN    std_logic; 
         clk_enable                      :   IN    std_logic; 
         reset                           :   IN    std_logic; 
         filter_in                       :   IN    std_logic_vector(15 DOWNTO 0); -- sfix16_En15
         filter_out                      :   OUT   std_logic_vector(27 DOWNTO 0); -- sfix28_En15
         ce_out                          :   OUT   std_logic  
         );

END tx_cic;


----------------------------------------------------------------
--Module Architecture: tx_cic
----------------------------------------------------------------
ARCHITECTURE rtl OF tx_cic IS
  -- Local Functions
  -- Type Definitions
  -- Constants
  CONSTANT zeroconst                      : signed(16 DOWNTO 0) := to_signed(0, 17); -- sfix17_En15
  -- Signals
  SIGNAL cur_count                        : unsigned(11 DOWNTO 0); -- ufix12
  SIGNAL phase_0                          : std_logic; -- boolean
  --   
  SIGNAL input_register                   : signed(15 DOWNTO 0); -- sfix16_En15
  --   -- Section 1 Signals 
  SIGNAL section_in1                      : signed(15 DOWNTO 0); -- sfix16_En15
  SIGNAL section_cast1                    : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL diff1                            : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL section_out1                     : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_cast                         : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_cast_1                       : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_temp                         : signed(17 DOWNTO 0); -- sfix18_En15
  SIGNAL cic_pipeline1                    : signed(16 DOWNTO 0); -- sfix17_En15
  --   -- Section 2 Signals 
  SIGNAL section_in2                      : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL diff2                            : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL section_out2                     : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_cast_2                       : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_cast_3                       : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sub_temp_1                       : signed(17 DOWNTO 0); -- sfix18_En15
  SIGNAL cic_pipeline2                    : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL upsampling                       : signed(16 DOWNTO 0); -- sfix17_En15
  --   -- Section 3 Signals 
  SIGNAL section_in3                      : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL sum1                             : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL section_out3                     : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL add_cast                         : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL add_cast_1                       : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL add_temp                         : signed(17 DOWNTO 0); -- sfix18_En15
  --   -- Section 4 Signals 
  SIGNAL section_in4                      : signed(16 DOWNTO 0); -- sfix17_En15
  SIGNAL section_cast4                    : signed(27 DOWNTO 0); -- sfix28_En15
  SIGNAL sum2                             : signed(27 DOWNTO 0); -- sfix28_En15
  SIGNAL section_out4                     : signed(27 DOWNTO 0); -- sfix28_En15
  SIGNAL add_cast_2                       : signed(27 DOWNTO 0); -- sfix28_En15
  SIGNAL add_cast_3                       : signed(27 DOWNTO 0); -- sfix28_En15
  SIGNAL add_temp_1                       : signed(28 DOWNTO 0); -- sfix29_En15
  --   
  SIGNAL output_register                  : signed(27 DOWNTO 0); -- sfix28_En15


BEGIN

  -- Block Statements
  --   ------------------ CE Output Generation ------------------

  ce_output : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      cur_count <= to_unsigned(0, 12);
    ELSIF clk'event AND clk = '1' THEN
      IF clk_enable = '1' THEN
        IF cur_count >= to_unsigned(2559, 12) THEN
          cur_count <= to_unsigned(0, 12);
        ELSE
          cur_count <= cur_count + to_unsigned(1, 12);
        END IF;
      END IF;
    END IF; 
  END PROCESS ce_output;

  phase_0 <= '1' WHEN cur_count = to_unsigned(0, 12) AND clk_enable = '1' ELSE '0';

  --   ------------------ Input Register ------------------

  input_reg_process : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      input_register <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF phase_0 = '1' THEN
        input_register <= signed(filter_in);
      END IF;
    END IF; 
  END PROCESS input_reg_process;

  --   ------------------ Section # 1 : Comb ------------------

  section_in1 <= input_register;

  section_cast1 <= resize(section_in1, 17);

  sub_cast <= section_cast1;
  sub_cast_1 <= diff1;
  sub_temp <= resize(sub_cast, 18) - resize(sub_cast_1, 18);
  section_out1 <= sub_temp(16 DOWNTO 0);

  comb_delay_section1 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      diff1 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF phase_0 = '1' THEN
        diff1 <= section_cast1;
      END IF;
    END IF; 
  END PROCESS comb_delay_section1;

  cic_pipeline_process_section1 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      cic_pipeline1 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF phase_0 = '1' THEN
        cic_pipeline1 <= section_out1;
      END IF;
    END IF; 
  END PROCESS cic_pipeline_process_section1;

  --   ------------------ Section # 2 : Comb ------------------

  section_in2 <= cic_pipeline1;

  sub_cast_2 <= section_in2;
  sub_cast_3 <= diff2;
  sub_temp_1 <= resize(sub_cast_2, 18) - resize(sub_cast_3, 18);
  section_out2 <= sub_temp_1(16 DOWNTO 0);

  comb_delay_section2 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      diff2 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF phase_0 = '1' THEN
        diff2 <= section_in2;
      END IF;
    END IF; 
  END PROCESS comb_delay_section2;

  cic_pipeline_process_section2 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      cic_pipeline2 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF phase_0 = '1' THEN
        cic_pipeline2 <= section_out2;
      END IF;
    END IF; 
  END PROCESS cic_pipeline_process_section2;

  upsampling <= cic_pipeline2 WHEN ( phase_0 = '1' ) ELSE
                zeroconst;
  --   ------------------ Section # 3 : Integrator ------------------

  section_in3 <= upsampling;

  add_cast <= section_in3;
  add_cast_1 <= section_out3;
  add_temp <= resize(add_cast, 18) + resize(add_cast_1, 18);
  sum1 <= add_temp(16 DOWNTO 0);

  integrator_delay_section3 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      section_out3 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF clk_enable = '1' THEN
        section_out3 <= sum1;
      END IF;
    END IF; 
  END PROCESS integrator_delay_section3;

  --   ------------------ Section # 4 : Integrator ------------------

  section_in4 <= section_out3;

  section_cast4 <= resize(section_in4, 28);

  add_cast_2 <= section_cast4;
  add_cast_3 <= section_out4;
  add_temp_1 <= resize(add_cast_2, 29) + resize(add_cast_3, 29);
  sum2 <= add_temp_1(27 DOWNTO 0);

  integrator_delay_section4 : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      section_out4 <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF clk_enable = '1' THEN
        section_out4 <= sum2;
      END IF;
    END IF; 
  END PROCESS integrator_delay_section4;

  --   ------------------ Output Register ------------------

  output_reg_process : PROCESS (clk, reset)
  BEGIN
    IF reset = '1' THEN
      output_register <= (OTHERS => '0');
    ELSIF clk'event AND clk = '1' THEN
      IF clk_enable = '1' THEN
        output_register <= section_out4;
      END IF;
    END IF; 
  END PROCESS output_reg_process;

  -- Assignment Statements
  ce_out <= phase_0;
  filter_out <= std_logic_vector(output_register);
END rtl;
