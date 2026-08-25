namespace EVG_Updater;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            ApplicationConfiguration.Initialize();
            Application.Run(new MainForm());
            return 0;
        }

        string cmd = args[0].ToLowerInvariant();
        string[] rest = args.Skip(1).ToArray();

        return cmd switch
        {
            "flash"         => RunFlashCli(rest).GetAwaiter().GetResult(),
            "flashbl"       => RunFlashBlCli(rest).GetAwaiter().GetResult(),
            "scan"          => RunScanCli(rest).GetAwaiter().GetResult(),
            "--help" or "-h" or "help" => PrintTopLevelUsage(0),
            _               => PrintTopLevelUsage(1, $"Unknown command: {args[0]}"),
        };
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  scan
    // ─────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Probes shorts 0..63, then pulls bank-0 identity + random address from
    /// each present gear. Prints the same columns as the GUI grid.
    /// </summary>
    static async Task<int> RunScanCli(string[] args)
    {
        string gatewayIp = "192.168.178.131";
        string oursGtinHex = "3452334E0CAD";
        bool quiet = false;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--ip" when i + 1 < args.Length:
                    gatewayIp = args[++i];
                    break;
                case "--ours-gtin" when i + 1 < args.Length:
                    oursGtinHex = args[++i];
                    break;
                case "--quiet" or "-q":
                    quiet = true;
                    break;
                case "--help" or "-h":
                    PrintScanUsage();
                    return 0;
                default:
                    Console.Error.WriteLine($"Unknown option: {args[i]}");
                    PrintScanUsage();
                    return 1;
            }
        }

        byte[] oursGtin;
        try
        {
            oursGtin = Convert.FromHexString(oursGtinHex.Replace(" ", "").Replace("0x", ""));
            if (oursGtin.Length != 6) throw new FormatException();
        }
        catch
        {
            Console.Error.WriteLine("ERROR: --ours-gtin must be 6 bytes hex (e.g. 3452334E0CAD)");
            return 1;
        }

        if (!quiet)
        {
            Console.WriteLine("DALI Bus Scan");
            Console.WriteLine($"  Gateway:   ws://{gatewayIp}");
            Console.WriteLine($"  Ours-GTIN: {oursGtinHex}");
            Console.WriteLine();
        }

        using var gateway = new DaliGateway();
        if (!quiet)
            gateway.OnLog += msg => Console.WriteLine($"  [{DateTime.Now:HH:mm:ss.fff}] {msg}");

        try
        {
            await gateway.ConnectAsync(gatewayIp);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ERROR: Connection failed: {ex.Message}");
            return 1;
        }

        var scanner = new DaliBusScanner(gateway);
        if (!quiet) scanner.OnLog += msg => Console.WriteLine($"  {msg}");

        List<ScannedDevice> devices;
        try
        {
            devices = await scanner.ScanAsync();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ERROR: Scan failed: {ex.Message}");
            await gateway.DisconnectAsync();
            return 2;
        }

        await gateway.DisconnectAsync();

        if (!quiet) Console.WriteLine();
        PrintScanTable(devices, oursGtin);
        if (!quiet) Console.WriteLine($"\n{devices.Count} gear listed.");

        return 0;
    }

    static void PrintScanTable(List<ScannedDevice> devices, byte[] oursGtin)
    {
        // Column order mirrors the GUI grid: Short | Random | GTIN | Mode | DT | FW | HW | Updatable
        string[] headers = { "Short", "Random", "GTIN", "Mode", "DT", "FW", "HW", "Updatable" };
        var rows = new List<string[]>();
        foreach (var d in devices)
        {
            rows.Add(new[]
            {
                d.ShortAddress.ToString(),
                d.RandomHex,
                d.GtinHex,
                d.ModeLabel,
                d.DeviceTypeLabel,
                d.FwVersion,
                d.HwVersion,
                d.IsOurs(oursGtin) ? "yes" : "no",
            });
        }

        var widths = new int[headers.Length];
        for (int c = 0; c < headers.Length; c++)
        {
            widths[c] = headers[c].Length;
            foreach (var r in rows)
                if (r[c].Length > widths[c]) widths[c] = r[c].Length;
        }

        string FormatRow(string[] cells)
        {
            var parts = new string[cells.Length];
            for (int c = 0; c < cells.Length; c++)
                parts[c] = cells[c].PadRight(widths[c]);
            return string.Join("  ", parts).TrimEnd();
        }

        Console.WriteLine(FormatRow(headers));
        Console.WriteLine(string.Join("  ", widths.Select(w => new string('-', w))));
        foreach (var r in rows) Console.WriteLine(FormatRow(r));
    }

    static void PrintScanUsage()
    {
        Console.WriteLine("Usage: EVG_Updater scan [options]");
        Console.WriteLine();
        Console.WriteLine("Probe the DALI bus (shorts 0..63) and print the discovered gear,");
        Console.WriteLine("using the same columns as the GUI grid.");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  --ip <gateway_ip>    Gateway IP (default: 192.168.178.131)");
        Console.WriteLine("  --ours-gtin <hex>    GTIN to flag as 'ours' (default: 3452334E0CAD)");
        Console.WriteLine("  --quiet, -q          Suppress progress log, print only the result table");
        Console.WriteLine("  --help, -h           Show this help");
        Console.WriteLine();
        Console.WriteLine("Example:");
        Console.WriteLine("  EVG_Updater scan --ip 192.168.178.131 --quiet");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  flash
    // ─────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Flashes a firmware image to one EVG on the DALI bus via the gateway.
    /// </summary>
    static async Task<int> RunFlashCli(string[] args)
    {
        string? firmwarePath = null;
        string gatewayIp = "192.168.178.131";
        byte shortAddr = 0;
        string gtinHex = "3452334E0CAD";
        byte evgMode = 5;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--ip" when i + 1 < args.Length:
                    gatewayIp = args[++i];
                    break;
                case "--addr" when i + 1 < args.Length:
                    if (!byte.TryParse(args[++i], out shortAddr) || shortAddr > 63)
                    {
                        Console.Error.WriteLine("ERROR: --addr must be 0-63");
                        return 1;
                    }
                    break;
                case "--gtin" when i + 1 < args.Length:
                    gtinHex = args[++i];
                    break;
                case "--mode" when i + 1 < args.Length:
                    if (!byte.TryParse(args[++i], out evgMode) || evgMode < 1 || evgMode > 8)
                    {
                        Console.Error.WriteLine("ERROR: --mode must be 1-8");
                        return 1;
                    }
                    break;
                case "--help" or "-h":
                    PrintFlashUsage();
                    return 0;
                default:
                    if (!args[i].StartsWith("--") && firmwarePath == null)
                        firmwarePath = args[i];
                    else
                    {
                        Console.Error.WriteLine($"Unknown option: {args[i]}");
                        PrintFlashUsage();
                        return 1;
                    }
                    break;
            }
        }

        if (firmwarePath == null)
        {
            Console.Error.WriteLine("ERROR: No firmware file specified");
            PrintFlashUsage();
            return 1;
        }

        if (!File.Exists(firmwarePath))
        {
            Console.Error.WriteLine($"ERROR: File not found: {firmwarePath}");
            return 1;
        }

        byte[] gtin;
        try
        {
            gtin = Convert.FromHexString(gtinHex.Replace(" ", "").Replace("0x", ""));
            if (gtin.Length != 6) throw new FormatException();
        }
        catch
        {
            Console.Error.WriteLine("ERROR: --gtin must be 6 bytes hex (e.g. 3452334E0CAD)");
            return 1;
        }

        var firmware = await File.ReadAllBytesAsync(firmwarePath);
        if (firmware.Length > 32640)
        {
            Console.Error.WriteLine($"ERROR: Firmware too large ({firmware.Length} bytes, max 32640)");
            return 1;
        }

        Console.WriteLine("DALI Firmware Flash");
        Console.WriteLine($"  Gateway:  ws://{gatewayIp}");
        Console.WriteLine($"  Firmware: {firmwarePath} ({firmware.Length} bytes)");
        Console.WriteLine($"  Address:  {shortAddr}");
        Console.WriteLine($"  GTIN:     {gtinHex}");
        Console.WriteLine($"  EVG Mode: {evgMode}");
        Console.WriteLine();

        using var gateway = new DaliGateway();
        gateway.OnLog += msg => Console.WriteLine($"  [{DateTime.Now:HH:mm:ss.fff}] {msg}");

        try
        {
            await gateway.ConnectAsync(gatewayIp);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ERROR: Connection failed: {ex.Message}");
            return 1;
        }

        var bootloader = new DaliBootloader(gateway);
        bootloader.OnLog += msg => Console.WriteLine($"  {msg}");
        bootloader.OnProgress += (cur, total) =>
        {
            if (total > 0)
            {
                int pct = cur * 100 / total;
                Console.Write($"\r  Progress: {pct}% ({cur}/{total} frames)");
                if (cur == total) Console.WriteLine();
            }
        };

        var success = await bootloader.UpdateFirmwareAsync(firmware, shortAddr, gtin, evgMode);

        await gateway.DisconnectAsync();

        if (success)
        {
            Console.WriteLine("\nSUCCESS: Firmware update complete.");
            return 0;
        }
        else
        {
            Console.Error.WriteLine("\nFAILED: Firmware update did not complete.");
            return 2;
        }
    }

    static void PrintFlashUsage()
    {
        Console.WriteLine("Usage: EVG_Updater flash <firmware.bin> [options]");
        Console.WriteLine();
        Console.WriteLine("Flash a firmware image to one EVG on the DALI bus via the gateway.");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  --ip <gateway_ip>    Gateway IP (default: 192.168.178.131)");
        Console.WriteLine("  --addr <0-63>        DALI short address (default: 0)");
        Console.WriteLine("  --gtin <hex>         6-byte GTIN hex (default: 3452334E0CAD)");
        Console.WriteLine("  --mode <1-8>         EVG mode ID (default: 5 = RGBW)");
        Console.WriteLine("  --help, -h           Show this help");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  EVG_Updater flash firmware.bin");
        Console.WriteLine("  EVG_Updater flash firmware.bin --addr 5 --mode 4");
        Console.WriteLine("  EVG_Updater flash firmware.bin --ip 10.0.0.50 --gtin 091201234567");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  flashbl
    // ─────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Step 1 of the bootloader emergency procedure: flash the BL emergency
    /// flasher firmware, which carries the bootloader compiled in and writes
    /// the boot area itself on boot. This is an ordinary firmware update —
    /// the bootloader image never travels over the bus.
    /// </summary>
    static async Task<int> RunFlashBlCli(string[] args)
    {
        string? flasherPath = null;
        string gatewayIp = "192.168.178.131";
        byte shortAddr = 0;
        string gtinHex = "3452334E0CAD";
        byte evgMode = 5;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--ip" when i + 1 < args.Length:
                    gatewayIp = args[++i];
                    break;
                case "--addr" when i + 1 < args.Length:
                    if (!byte.TryParse(args[++i], out shortAddr) || shortAddr > 63)
                    {
                        Console.Error.WriteLine("ERROR: --addr must be 0-63");
                        return 1;
                    }
                    break;
                case "--gtin" when i + 1 < args.Length:
                    gtinHex = args[++i];
                    break;
                case "--mode" when i + 1 < args.Length:
                    if (!byte.TryParse(args[++i], out evgMode) || evgMode < 1 || evgMode > 8)
                    {
                        Console.Error.WriteLine("ERROR: --mode must be 1-8");
                        return 1;
                    }
                    break;
                case "--help" or "-h":
                    PrintFlashBlUsage();
                    return 0;
                default:
                    if (!args[i].StartsWith("--") && flasherPath == null)
                        flasherPath = args[i];
                    else
                    {
                        Console.Error.WriteLine($"Unknown option: {args[i]}");
                        PrintFlashBlUsage();
                        return 1;
                    }
                    break;
            }
        }

        flasherPath ??= DaliBootloader.FindEmergencyFlasher();
        if (flasherPath == null)
        {
            Console.Error.WriteLine(
                "ERROR: BL emergency flasher image not found. Pass it explicitly, or place\n" +
                $"       '{DaliBootloader.EmergencyFlasherFileName}' next to the executable.\n" +
                "       Build it with: cd BL-Emergency-Flasher && pio run");
            return 1;
        }
        if (!File.Exists(flasherPath))
        {
            Console.Error.WriteLine($"ERROR: File not found: {flasherPath}");
            return 1;
        }

        byte[] gtin;
        try
        {
            gtin = Convert.FromHexString(gtinHex.Replace(" ", "").Replace("0x", ""));
            if (gtin.Length != 6) throw new FormatException();
        }
        catch
        {
            Console.Error.WriteLine("ERROR: --gtin must be 6 bytes hex (e.g. 3452334E0CAD)");
            return 1;
        }

        var flasher = await File.ReadAllBytesAsync(flasherPath);

        Console.WriteLine("DALI Bootloader Emergency Flash (step 1 of 2)");
        Console.WriteLine($"  Gateway:  ws://{gatewayIp}");
        Console.WriteLine($"  Flasher:  {flasherPath} ({flasher.Length} bytes)");
        Console.WriteLine($"  Address:  {shortAddr}");
        Console.WriteLine($"  GTIN:     {gtinHex}");
        Console.WriteLine($"  EVG Mode: {evgMode}");
        Console.WriteLine();
        Console.WriteLine("  The flasher carries the bootloader compiled in and writes the");
        Console.WriteLine("  boot area itself once it boots. Watch UART (PD5, 115200) for");
        Console.WriteLine("  'BL FLASH OK'. Afterwards flash the application firmware back:");
        Console.WriteLine($"    EVG_Updater flash firmware.bin --addr {shortAddr}");
        Console.WriteLine();

        using var gateway = new DaliGateway();
        gateway.OnLog += msg => Console.WriteLine($"  [{DateTime.Now:HH:mm:ss.fff}] {msg}");

        try
        {
            await gateway.ConnectAsync(gatewayIp);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ERROR: Connection failed: {ex.Message}");
            return 1;
        }

        var bootloader = new DaliBootloader(gateway);
        bootloader.OnLog += msg => Console.WriteLine($"  {msg}");
        bootloader.OnProgress += (cur, total) =>
        {
            if (total > 0)
            {
                int pct = cur * 100 / total;
                Console.Write($"\r  Progress: {pct}% ({cur}/{total} frames)");
                if (cur == total) Console.WriteLine();
            }
        };

        var success = await bootloader.UpdateFirmwareAsync(flasher, shortAddr, gtin, evgMode);

        await gateway.DisconnectAsync();

        if (success)
        {
            Console.WriteLine("\nSUCCESS: Emergency flasher installed and running.");
            Console.WriteLine("Check UART for 'BL FLASH OK', then flash the application firmware back.");
            return 0;
        }
        Console.Error.WriteLine("\nFAILED: Emergency flasher was not installed.");
        return 2;
    }

    static void PrintFlashBlUsage()
    {
        Console.WriteLine("Usage: EVG_Updater flashbl [flasher.bin] [options]");
        Console.WriteLine();
        Console.WriteLine("Replace the DALI bootloader of one EVG (emergency procedure).");
        Console.WriteLine();
        Console.WriteLine("The CH32V003 boot area cannot be written over the bus by the");
        Console.WriteLine("application firmware, so this flashes a dedicated firmware that has");
        Console.WriteLine("the bootloader compiled in and programs the boot area itself. It is");
        Console.WriteLine("an ordinary firmware update; the device reboots into it and reports");
        Console.WriteLine("the result on UART (PD5, 115200).");
        Console.WriteLine();
        Console.WriteLine("Full procedure:");
        Console.WriteLine("  1. EVG_Updater flashbl --addr <n>       <- this command");
        Console.WriteLine("  2. wait for 'BL FLASH OK' on UART");
        Console.WriteLine("  3. EVG_Updater flash firmware.bin --addr <n>");
        Console.WriteLine();
        Console.WriteLine($"Without a path argument, '{DaliBootloader.EmergencyFlasherFileName}' is");
        Console.WriteLine("looked up next to the executable (and in the dev build tree).");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  --ip <gateway_ip>    Gateway IP (default: 192.168.178.131)");
        Console.WriteLine("  --addr <0-63>        DALI short address (default: 0)");
        Console.WriteLine("  --gtin <hex>         6-byte GTIN hex (default: 3452334E0CAD)");
        Console.WriteLine("  --mode <1-8>         EVG mode ID (default: 5 = RGBW)");
        Console.WriteLine("  --help, -h           Show this help");
        Console.WriteLine();
        Console.WriteLine("Example:");
        Console.WriteLine("  EVG_Updater flashbl --addr 1");
        return;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  top-level help
    // ─────────────────────────────────────────────────────────────────────────

    static int PrintTopLevelUsage(int exitCode, string? errorMessage = null)
    {
        if (errorMessage != null)
            Console.Error.WriteLine($"ERROR: {errorMessage}\n");

        Console.WriteLine("Usage: EVG_Updater <command> [options]");
        Console.WriteLine();
        Console.WriteLine("Commands:");
        Console.WriteLine("  flash <firmware.bin>    Flash a firmware image to an EVG via DALI bus");
        Console.WriteLine("  flashbl <bootldr.bin>   Update the EVG's DALI bootloader over the bus");
        Console.WriteLine("  scan                    Probe the DALI bus and print discovered gear");
        Console.WriteLine();
        Console.WriteLine("Run 'EVG_Updater <command> --help' for details on each command.");
        Console.WriteLine("Run without arguments to launch the GUI.");
        return exitCode;
    }
}
