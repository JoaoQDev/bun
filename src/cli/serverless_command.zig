const std = @import("std");
const bun = @import("bun");
const Output = bun.Output;
const Global = bun.Global;
const strings = bun.strings;

pub const ServerlessCommand = struct {
    extern fn bun_serverless_main(config_path: [*:0]const u8, port: c_int) c_int;

    pub fn exec(allocator: std.mem.Allocator) !void {
        _ = allocator;
        var config_path: ?[:0]const u8 = null;
        var port: u16 = 3000;
        var show_help = false;

        var args_iter = ArgsIterator{ .buf = bun.argv };
        _ = args_iter.next(); // skip executable
        _ = args_iter.next(); // skip "serverless"

        while (args_iter.next()) |arg| {
            if (strings.eqlComptime(arg, "--help") or strings.eqlComptime(arg, "-h")) {
                show_help = true;
            } else if (strings.eqlComptime(arg, "--config")) {
                config_path = args_iter.next();
            } else if (strings.eqlComptime(arg, "--port")) {
                if (args_iter.next()) |port_str| {
                    port = std.fmt.parseInt(u16, port_str, 10) catch {
                        Output.prettyErrorln("<r><red>error<r>: invalid port number: {s}", .{port_str});
                        Global.exit(1);
                    };
                }
            } else if (arg.len > 0 and arg[0] != '-') {
                // Positional argument: treat as config path
                if (config_path == null) {
                    config_path = arg;
                }
            }
        }

        if (show_help) {
            printHelp();
            return;
        }

        const resolved_config = config_path orelse "./workers.json";

        const result = bun_serverless_main(resolved_config.ptr, @as(c_int, @intCast(port)));
        if (result != 0) {
            Global.exit(1);
        }
    }

    fn printHelp() void {
        Output.pretty(
            \\<b>Usage<r>: <b><green>bun serverless<r> <cyan>[flags]<r> <blue>[config]<r>
            \\
            \\Start a serverless runtime that routes HTTP requests to JavaScript/TypeScript workers.
            \\
            \\<b>Flags:<r>
            \\  <cyan>--config<r> FILE      Path to workers.json config file (default: ./workers.json)
            \\  <cyan>--port<r> NUMBER      Port to listen on (default: 3000)
            \\  <cyan>-h, --help<r>         Show this help message
            \\
            \\<b>Examples:<r>
            \\  <green>bun serverless<r>
            \\  <green>bun serverless ./workers.json<r>
            \\  <green>bun serverless --config ./workers.json --port 8080<r>
            \\
        , .{});
        Output.flush();
    }

    const ArgsIterator = struct {
        buf: []const [:0]const u8,
        i: usize = 0,

        pub fn next(self: *ArgsIterator) ?[:0]const u8 {
            if (self.i >= self.buf.len) return null;
            const arg = self.buf[self.i];
            self.i += 1;
            return arg;
        }
    };
};
