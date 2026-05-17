namespace EVG_Updater;

partial class MainForm
{
    private System.ComponentModel.IContainer components = null;

    protected override void Dispose(bool disposing)
    {
        if (disposing && (components != null))
            components.Dispose();
        base.Dispose(disposing);
    }

    #region Windows Form Designer generated code

    private void InitializeComponent()
    {
        // Connection group
        grpConnection = new GroupBox();
        lblGatewayIp = new Label();
        txtGatewayIp = new TextBox();
        btnConnect = new Button();

        // Bus devices group
        grpDevices = new GroupBox();
        gridDevices = new DataGridView();
        btnRescan = new Button();
        lblOursGtin = new Label();
        txtOursGtin = new TextBox();

        // Update group
        grpUpdate = new GroupBox();
        lblFirmware = new Label();
        txtFirmwarePath = new TextBox();
        btnBrowse = new Button();
        lblAddress = new Label();
        txtShortAddress = new TextBox();
        lblGtin = new Label();
        txtGtin = new TextBox();
        lblEvgMode = new Label();
        numEvgMode = new NumericUpDown();
        btnStartUpdate = new Button();
        btnCancel = new Button();
        progressBar = new ProgressBar();

        // Log
        txtLog = new TextBox();
        btnClearLog = new Button();

        SuspendLayout();

        // === Connection Group ===
        grpConnection.Text = "Gateway Connection";
        grpConnection.Location = new Point(12, 12);
        grpConnection.Size = new Size(560, 60);

        lblGatewayIp.Text = "IP:";
        lblGatewayIp.Location = new Point(10, 24);
        lblGatewayIp.AutoSize = true;

        txtGatewayIp.Text = "192.168.178.131";
        txtGatewayIp.Location = new Point(30, 21);
        txtGatewayIp.Size = new Size(150, 23);

        btnConnect.Text = "Connect";
        btnConnect.Location = new Point(190, 20);
        btnConnect.Size = new Size(90, 25);
        btnConnect.Click += btnConnect_Click;

        grpConnection.Controls.AddRange(new Control[] { lblGatewayIp, txtGatewayIp, btnConnect });

        // === Bus Devices Group ===
        grpDevices.Text = "Bus Devices (double-click a row to load)";
        grpDevices.Location = new Point(12, 80);
        grpDevices.Size = new Size(560, 200);
        grpDevices.Enabled = false;

        lblOursGtin.Text = "Ours GTIN:";
        lblOursGtin.Location = new Point(10, 22);
        lblOursGtin.AutoSize = true;

        txtOursGtin.Text = "3452334E0CAD";
        txtOursGtin.Location = new Point(80, 19);
        txtOursGtin.Size = new Size(130, 23);

        btnRescan.Text = "Rescan";
        btnRescan.Location = new Point(220, 18);
        btnRescan.Size = new Size(80, 25);
        btnRescan.Click += btnRescan_Click;

        gridDevices.Location = new Point(10, 50);
        gridDevices.Size = new Size(540, 140);
        gridDevices.AllowUserToAddRows = false;
        gridDevices.AllowUserToDeleteRows = false;
        gridDevices.ReadOnly = true;
        gridDevices.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
        gridDevices.MultiSelect = false;
        gridDevices.RowHeadersVisible = false;
        gridDevices.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colShort",  HeaderText = "Short",     FillWeight = 50 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colRandom", HeaderText = "Long Addr", FillWeight = 90 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colGtin",   HeaderText = "GTIN",      FillWeight = 130 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colMode",   HeaderText = "Mode",      FillWeight = 80 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colDt",     HeaderText = "DT",        FillWeight = 40 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colFw",     HeaderText = "FW",        FillWeight = 50 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colHw",     HeaderText = "HW",        FillWeight = 50 });
        gridDevices.Columns.Add(new DataGridViewTextBoxColumn { Name = "colOurs",   HeaderText = "Updatable", FillWeight = 70 });
        gridDevices.CellDoubleClick += gridDevices_CellDoubleClick;

        grpDevices.Controls.AddRange(new Control[] {
            lblOursGtin, txtOursGtin, btnRescan, gridDevices
        });

        // === Update Group ===
        grpUpdate.Text = "Firmware Update";
        grpUpdate.Location = new Point(12, 290);
        grpUpdate.Size = new Size(560, 180);
        grpUpdate.Enabled = false;

        lblFirmware.Text = "Firmware:";
        lblFirmware.Location = new Point(10, 25);
        lblFirmware.AutoSize = true;

        txtFirmwarePath.Location = new Point(80, 22);
        txtFirmwarePath.Size = new Size(370, 23);
        txtFirmwarePath.ReadOnly = true;

        btnBrowse.Text = "...";
        btnBrowse.Location = new Point(456, 21);
        btnBrowse.Size = new Size(35, 25);
        btnBrowse.Click += btnBrowse_Click;

        lblAddress.Text = "Address:";
        lblAddress.Location = new Point(10, 58);
        lblAddress.AutoSize = true;

        txtShortAddress.Text = "0";
        txtShortAddress.Location = new Point(80, 55);
        txtShortAddress.Size = new Size(40, 23);

        lblGtin.Text = "GTIN:";
        lblGtin.Location = new Point(140, 58);
        lblGtin.AutoSize = true;

        txtGtin.Text = "3452334E0CAD";
        txtGtin.Location = new Point(180, 55);
        txtGtin.Size = new Size(130, 23);

        lblEvgMode.Text = "EVG Mode:";
        lblEvgMode.Location = new Point(325, 58);
        lblEvgMode.AutoSize = true;

        numEvgMode.Minimum = 1;
        numEvgMode.Maximum = 8;
        numEvgMode.Value = 5; // RGBW default
        numEvgMode.Location = new Point(400, 55);
        numEvgMode.Size = new Size(50, 23);

        btnStartUpdate.Text = "Start Update";
        btnStartUpdate.Location = new Point(80, 95);
        btnStartUpdate.Size = new Size(120, 30);
        btnStartUpdate.Enabled = false;
        btnStartUpdate.Click += btnStartUpdate_Click;

        btnCancel.Text = "Cancel";
        btnCancel.Location = new Point(210, 95);
        btnCancel.Size = new Size(80, 30);
        btnCancel.Enabled = false;
        btnCancel.Click += btnCancel_Click;

        progressBar.Location = new Point(80, 140);
        progressBar.Size = new Size(410, 23);

        grpUpdate.Controls.AddRange(new Control[] {
            lblFirmware, txtFirmwarePath, btnBrowse,
            lblAddress, txtShortAddress,
            lblGtin, txtGtin,
            lblEvgMode, numEvgMode,
            btnStartUpdate, btnCancel, progressBar
        });

        // === Log ===
        txtLog.Location = new Point(12, 480);
        txtLog.Size = new Size(560, 200);
        txtLog.Multiline = true;
        txtLog.ReadOnly = true;
        txtLog.ScrollBars = ScrollBars.Vertical;
        txtLog.Font = new Font("Consolas", 9F);

        btnClearLog.Text = "Clear Log";
        btnClearLog.Location = new Point(12, 686);
        btnClearLog.Size = new Size(80, 25);
        btnClearLog.Click += btnClearLog_Click;

        // === Form ===
        AutoScaleDimensions = new SizeF(7F, 15F);
        AutoScaleMode = AutoScaleMode.Font;
        ClientSize = new Size(584, 721);
        Controls.AddRange(new Control[] { grpConnection, grpDevices, grpUpdate, txtLog, btnClearLog });
        Text = "DALI Firmware Updater";
        MinimumSize = new Size(600, 760);

        ResumeLayout(false);
    }

    #endregion

    private GroupBox grpConnection;
    private Label lblGatewayIp;
    private TextBox txtGatewayIp;
    private Button btnConnect;

    private GroupBox grpDevices;
    private DataGridView gridDevices;
    private Button btnRescan;
    private Label lblOursGtin;
    private TextBox txtOursGtin;

    private GroupBox grpUpdate;
    private Label lblFirmware;
    private TextBox txtFirmwarePath;
    private Button btnBrowse;
    private Label lblAddress;
    private TextBox txtShortAddress;
    private Label lblGtin;
    private TextBox txtGtin;
    private Label lblEvgMode;
    private NumericUpDown numEvgMode;
    private Button btnStartUpdate;
    private Button btnCancel;
    private ProgressBar progressBar;

    private TextBox txtLog;
    private Button btnClearLog;
}
