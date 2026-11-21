/*
 * Copyright 2020 Claude Schwarz
 * Copyright 2020 Niklas Ekström - rewrite in Verilog
 *
 * "long data" variant: for accesses outside Chip RAM (address >= $200000,
 * i.e. A23..A21 != 0) the bus cycle is made closer to a real 68000:
 *  - address and write data are held HOLD_CYCLES c200m periods (5 ns each)
 *    after /AS, /UDS and /LDS are released;
 *  - at least 4 CPU clocks (8 c7m edges) separate two consecutive /AS.
 * Chip RAM accesses keep the timing of the original pistorm.v.
 * Built with Quartus 20.1.1 as module "pistorm" (copy this file to pistorm.v
 * in the project folder). Target EPM240T100C5 with fitter SEED 7
 * (set_global_assignment -name SEED 7): best internal timing of seeds 1-8.
 */
module pistorm(
    output reg      PI_TXN_IN_PROGRESS, // GPIO0
    output reg      PI_IPL_ZERO,        // GPIO1
    input   [1:0]   PI_A,       // GPIO[3..2]
    input           PI_CLK,     // GPIO4
    output reg      PI_RESET,   // GPIO5
    input           PI_RD,      // GPIO6
    input           PI_WR,      // GPIO7
    inout   [15:0]  PI_D,       // GPIO[23..8]

    output reg      LTCH_A_0,
    output reg      LTCH_A_8,
    output reg      LTCH_A_16,
    output reg      LTCH_A_24,
    output reg      LTCH_A_OE_n,
    output reg      LTCH_D_RD_U,
    output reg      LTCH_D_RD_L,
    output reg      LTCH_D_RD_OE_n,
    output reg      LTCH_D_WR_U,
    output reg      LTCH_D_WR_L,
    output reg      LTCH_D_WR_OE_n,

    input           M68K_CLK,
    output  reg [2:0] M68K_FC,

    output reg      M68K_AS_n,
    output reg      M68K_UDS_n,
    output reg      M68K_LDS_n,
    output reg      M68K_RW,

    input           M68K_DTACK_n,
    input           M68K_BERR_n,

    input           M68K_VPA_n,
    output reg      M68K_E,
    output reg      M68K_VMA_n,

    input   [2:0]   M68K_IPL_n,

    inout           M68K_RESET_n,
    inout           M68K_HALT_n,

    input           M68K_BR_n,
    output reg      M68K_BG_n,
    input           M68K_BGACK_n,
    input           M68K_C1,
    input           M68K_C3,
    input           CLK_SEL
  );

  wire c200m = PI_CLK;
  reg [2:0] c7m_sync;
//  wire c7m = M68K_CLK;
  wire c7m = c7m_sync[2];
  wire c1c3_clk = !(M68K_C1 ^ M68K_C3);

  localparam REG_DATA = 2'd0;
  localparam REG_ADDR_LO = 2'd1;
  localparam REG_ADDR_HI = 2'd2;
  localparam REG_STATUS = 2'd3;

  initial begin
    PI_TXN_IN_PROGRESS <= 1'b0;
    PI_IPL_ZERO <= 1'b0;

    PI_RESET <= 1'b0;

    M68K_FC <= 3'd0;

    M68K_RW <= 1'b1;

    M68K_E <= 1'b0;
    M68K_VMA_n <= 1'b1;

    M68K_BG_n <= 1'b1;
  end

  reg [1:0] rd_sync;
  reg [1:0] wr_sync;

  always @(posedge c200m) begin
    rd_sync <= {rd_sync[0], PI_RD};
    wr_sync <= {wr_sync[0], PI_WR};
  end

  wire rd_rising = !rd_sync[1] && rd_sync[0];
  wire wr_rising = !wr_sync[1] && wr_sync[0];

  reg [15:0] data_out;
  assign PI_D = PI_A == REG_STATUS && PI_RD ? data_out : 16'bz;

  always @(posedge c200m) begin
    if (rd_rising && PI_A == REG_STATUS) begin
      data_out <= {ipl, 13'd0};
    end
  end

  reg [15:0] status;
  wire reset_out = !status[1];

  assign M68K_RESET_n = reset_out ? 1'b0 : 1'bz;
  assign M68K_HALT_n = reset_out ? 1'b0 : 1'bz;

  reg op_req = 1'b0;
  reg op_rw = 1'b1;
  reg op_uds_n = 1'b1;
  reg op_lds_n = 1'b1;

  // Long-data additions
  localparam [2:0] HOLD_CYCLES = 3'd4;  // data/address hold after /AS, 4 x 5 ns
  reg       op_slow = 1'b0;             // next access is outside Chip RAM
  reg       cur_slow = 1'b0;            // access in progress is outside Chip RAM
  reg [3:0] as_age = 4'd8;              // c7m edges since the last /AS fall
  reg       gap_ok = 1'b1;              // registered: 7 edges counted, next rising edge may start /AS
  reg       go_r = 1'b0;                // registered: request pending and gap rule satisfied
  reg [2:0] hold_cnt = 3'd0;

  always @(*) begin
    LTCH_D_WR_U <= PI_A == REG_DATA && PI_WR;
    LTCH_D_WR_L <= PI_A == REG_DATA && PI_WR;

    LTCH_A_0 <= PI_A == REG_ADDR_LO && PI_WR;
    LTCH_A_8 <= PI_A == REG_ADDR_LO && PI_WR;

    LTCH_A_16 <= PI_A == REG_ADDR_HI && PI_WR;
    LTCH_A_24 <= PI_A == REG_ADDR_HI && PI_WR;

    LTCH_D_RD_OE_n <= !(PI_A == REG_DATA && PI_RD);
  end

  reg a0;

  always @(posedge c200m) begin
    c7m_sync <= {c7m_sync[1:0], (CLK_SEL?M68K_CLK:c1c3_clk)};
  end

  // Edge detectors computed one stage earlier and registered: identical in
  // time to !c7m_sync[2] && c7m_sync[1] (resp. the falling one).
  reg c7m_rising = 1'b0;
  reg c7m_falling = 1'b0;

  always @(posedge c200m) begin
    c7m_rising <= !c7m_sync[1] && c7m_sync[0];
    c7m_falling <= c7m_sync[1] && !c7m_sync[0];
  end

  reg [2:0] ipl;
  reg [2:0] ipl_1;
  reg [2:0] ipl_2;

  always @(posedge c200m) begin
    if (c7m_falling) begin
      ipl_1 <= ~M68K_IPL_n;
      ipl_2 <= ipl_1;
    end

    if (ipl_2 == ipl_1)
      ipl <= ipl_2;

    PI_IPL_ZERO <= ipl == 3'd0;
  end

  always @(posedge c200m) begin
    PI_RESET <= reset_out ? 1'b1 : M68K_RESET_n;
  end

  reg [3:0] e_counter = 4'd0;

  always @(negedge c7m) begin
    if (e_counter == 4'd9)
      e_counter <= 4'd0;
    else
      e_counter <= e_counter + 4'd1;
  end

  always @(negedge c7m) begin
    if (e_counter == 4'd9)
      M68K_E <= 1'b0;
    else if (e_counter == 4'd5)
      M68K_E <= 1'b1;
  end

  reg [2:0] state = 3'd0;
  reg [2:0] PI_TXN_IN_PROGRESS_delay;

  always @(posedge c200m) begin

    if (wr_rising) begin
      case (PI_A)
        REG_ADDR_LO: begin
          a0 <= PI_D[0];
          PI_TXN_IN_PROGRESS <= 1'b1;
        end
        REG_ADDR_HI: begin
          op_req <= 1'b1;
          op_slow <= (PI_D[7:5] != 3'b000); // A23..A21 != 0 -> not Chip RAM
          op_rw <= PI_D[9];
          op_uds_n <= PI_D[8] ? a0 : 1'b0;
          op_lds_n <= PI_D[8] ? !a0 : 1'b0;
        end
        REG_STATUS: begin
          status <= PI_D;
        end
      endcase
    end

    // c7m edges since /AS fell (saturates at 8; cleared on S1 -> S2 below)
    if ((c7m_rising || c7m_falling) && as_age != 4'd8)
      as_age <= as_age + 4'd1;
    // Registered compare keeps the S1 exit condition shallow (200 MHz timing).
    gap_ok <= (as_age >= 4'd7);
    go_r <= op_req && (!op_slow || gap_ok);

    // Data/address hold counter, kept out of the state machine on purpose.
    // The next /AS is at least 3 clocks away, far more than HOLD_CYCLES.
    if (hold_cnt != 3'd0) begin
      hold_cnt <= hold_cnt - 3'd1;
      if (hold_cnt == 3'd1) begin
        LTCH_D_WR_OE_n <= 1'b1;
        LTCH_A_OE_n <= 1'b1;
      end
    end

    case (state)
      3'd0: begin // S0
        M68K_RW <= 1'b1; // S7 -> S0
//        if (c7m_falling) begin
//          if (op_req) begin
            state <= 2'd1;
//          end
//        end
      end

      3'd1: begin // S1
        // go_r: request pending and, for a non Chip RAM access, 7 c7m edges
        // since the previous /AS were already counted, so this rising edge
        // is the 8th, i.e. 4 clocks after the last /AS.
        if (go_r && c7m_rising) begin
          state <= 3'd2;
          as_age <= 4'd0;
        end
      end
      3'd2: begin // S2
        cur_slow <= op_slow;
        M68K_RW <= op_rw; // S1 -> S2
        LTCH_D_WR_OE_n <= op_rw;
        LTCH_A_OE_n <= 1'b0;
        M68K_AS_n <= 1'b0;
        M68K_UDS_n <= op_rw ? op_uds_n : 1'b1;
        M68K_LDS_n <= op_rw ? op_lds_n : 1'b1;
        if (c7m_falling) begin
          M68K_UDS_n <= op_uds_n;
          M68K_LDS_n <= op_lds_n;
          state <= 3'd3;
        end
      end

      3'd3: begin // S3
        op_req <= 1'b0;
        if(c7m_rising) begin
          if (!M68K_DTACK_n || (!M68K_VMA_n && e_counter == 4'd8)) begin
            state <= 3'd4;
            PI_TXN_IN_PROGRESS_delay[2:0] <= 3'b111;
          end
          else begin
            if (!M68K_VPA_n && e_counter == 4'd2) begin
              M68K_VMA_n <= 1'b0;
            end
          end
        end
      end
      3'd4: begin // S4
        PI_TXN_IN_PROGRESS_delay <= {PI_TXN_IN_PROGRESS_delay[1:0],1'b0};
        PI_TXN_IN_PROGRESS <= PI_TXN_IN_PROGRESS_delay[2];
        LTCH_D_RD_U <= 1'b1;
        LTCH_D_RD_L <= 1'b1;
        if (c7m_falling) begin
          state <= 3'd5;
          PI_TXN_IN_PROGRESS <= 1'b0;
        end
      end

      3'd5: begin // S5
        LTCH_D_RD_U <= 1'b0;
        LTCH_D_RD_L <= 1'b0;
        if (c7m_rising) begin
          state <= 3'd6;
        end
      end
       
      3'd6: begin // S6
        if (c7m_falling) begin
          M68K_VMA_n <= 1'b1;
          state <= 3'd7;
        end
      end
       
      3'd7: begin // S7
        M68K_AS_n <= 1'b1;
        M68K_UDS_n <= 1'b1;
        M68K_LDS_n <= 1'b1;
        if (cur_slow) begin
          // outside Chip RAM: address and write data stay driven for
          // HOLD_CYCLES more c200m periods (released by the counter above)
          hold_cnt <= HOLD_CYCLES;
        end
        else begin
          LTCH_D_WR_OE_n <= 1'b1;
          LTCH_A_OE_n <= 1'b1;
        end
        state <= 3'd0;
      end
    endcase
  end

endmodule
