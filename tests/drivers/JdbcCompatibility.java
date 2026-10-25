/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
import java.io.StringReader;
import java.io.StringWriter;
import java.math.BigDecimal;
import java.sql.*;
import java.time.LocalDate;
import java.time.OffsetDateTime;
import java.util.*;
import java.util.concurrent.*;
import org.postgresql.PGConnection;
import org.postgresql.PGNotification;

/** Exercises the ordinary, unmodified pgJDBC driver over TCP. */
public final class JdbcCompatibility {
    private static String url;
    private static String user;
    private static String password;
    private static int passed;

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

    private static Connection connect(String mode) throws SQLException {
        Properties properties = new Properties();
        properties.setProperty("user", user);
        properties.setProperty("password", password);
        properties.setProperty("sslmode", "disable");
        properties.setProperty("connectTimeout", "5");
        properties.setProperty("socketTimeout", "15");
        properties.setProperty("prepareThreshold", "5");
        properties.setProperty("preferQueryMode", mode);
        properties.setProperty("ApplicationName", "postgamma-jdbc-check");
        return DriverManager.getConnection(url, properties);
    }

    private static long scalar(Connection connection, String sql) throws SQLException {
        try (Statement statement = connection.createStatement();
             ResultSet rows = statement.executeQuery(sql)) {
            require(rows.next(), "scalar query returned no row");
            long value = rows.getLong(1);
            require(!rows.wasNull() && !rows.next(), "scalar query has the wrong shape");
            return value;
        }
    }

    private static void execute(Connection connection, String sql) throws SQLException {
        try (Statement statement = connection.createStatement()) {
            statement.execute(sql);
        }
    }

    private static void pass(String name) {
        passed++;
        System.out.println("JDBC_CHECK " + name);
    }

    private static void typedResults(Connection connection) throws Exception {
        execute(connection, "CREATE TEMP TABLE jdbc_types (id int PRIMARY KEY, "
                + "big bigint, num numeric(30,8), flag bool, txt text, raw bytea, "
                + "uid uuid, day date, stamp timestamptz, vals int[], doc jsonb, nil text)");
        long big = 9223372036854775800L;
        BigDecimal num = new BigDecimal("12345678901234567890.12345678");
        String text = "\u03bb-driver ' parameter";
        byte[] bytes = {0, 1, 127, (byte) 255};
        UUID uuid = UUID.fromString("00112233-4455-6677-8899-aabbccddeeff");
        LocalDate day = LocalDate.of(2024, 2, 29);
        OffsetDateTime stamp = OffsetDateTime.parse("2026-10-10T12:34:56.123456+03:00");
        try (PreparedStatement insert = connection.prepareStatement(
                "INSERT INTO jdbc_types VALUES (1,?,?,?,?,?,?,?,?,?,?::jsonb,?)")) {
            insert.setLong(1, big);
            insert.setBigDecimal(2, num);
            insert.setBoolean(3, true);
            insert.setString(4, text);
            insert.setBytes(5, bytes);
            insert.setObject(6, uuid);
            insert.setObject(7, day);
            insert.setObject(8, stamp);
            java.sql.Array array = connection.createArrayOf("int4", new Integer[]{1, null, 3});
            try {
                insert.setArray(9, array);
                insert.setString(10, "{\"label\":\"\\u03bb\"}");
                insert.setNull(11, Types.VARCHAR);
                require(insert.executeUpdate() == 1, "typed insert count");
            } finally {
                array.free();
            }
        }
        try (PreparedStatement select = connection.prepareStatement(
                "SELECT big,num,flag,txt,raw,uid,day,stamp,vals,doc->>'label',nil "
                + "FROM jdbc_types WHERE id=?")) {
            for (int i = 0; i < 10; i++) {
                select.setInt(1, 1);
                try (ResultSet row = select.executeQuery()) {
                    require(row.next(), "typed row absent");
                    require(row.getLong(1) == big, "bigint changed");
                    require(row.getBigDecimal(2).compareTo(num) == 0, "numeric changed");
                    require(row.getBoolean(3) && text.equals(row.getString(4)), "text/bool changed");
                    require(Arrays.equals(bytes, row.getBytes(5)), "bytea changed");
                    require(uuid.equals(row.getObject(6)), "uuid changed");
                    require(day.equals(row.getObject(7, LocalDate.class)), "date changed");
                    require(stamp.toInstant().equals(row.getObject(8, OffsetDateTime.class).toInstant()),
                            "timestamptz changed");
                    java.sql.Array array = row.getArray(9);
                    try {
                        require(Arrays.equals(new Integer[]{1, null, 3}, (Object[]) array.getArray()),
                                "array changed");
                    } finally {
                        array.free();
                    }
                    require("\u03bb".equals(row.getString(10)), "jsonb changed");
                    require(row.getString(11) == null && row.wasNull(), "SQL NULL changed");
                    require(!row.next(), "extra typed row");
                }
            }
        }
        require(scalar(connection, "SELECT count(*) FROM pg_prepared_statements "
                + "WHERE statement LIKE '%FROM jdbc_types%'") > 0, "server preparation not exercised");
        pass("typed_parameters_and_prepared_results");
    }

    private static void transactions(Connection connection) throws SQLException {
        execute(connection, "CREATE TEMP TABLE jdbc_tx (id int PRIMARY KEY)");
        connection.setAutoCommit(false);
        execute(connection, "INSERT INTO jdbc_tx VALUES (1)");
        connection.rollback();
        require(scalar(connection, "SELECT count(*) FROM jdbc_tx") == 0, "rollback failed");
        execute(connection, "INSERT INTO jdbc_tx VALUES (2)");
        Savepoint savepoint = connection.setSavepoint();
        try {
            execute(connection, "INSERT INTO jdbc_tx VALUES (2)");
            throw new IllegalStateException("duplicate key accepted");
        } catch (SQLException error) {
            require("23505".equals(error.getSQLState()), "duplicate key SQLSTATE changed");
        }
        connection.rollback(savepoint);
        connection.releaseSavepoint(savepoint);
        connection.commit();
        connection.setAutoCommit(true);
        require(scalar(connection, "SELECT sum(id) FROM jdbc_tx") == 2, "savepoint/commit failed");
        pass("transactions_and_savepoints");
    }

    private static void batch(Connection connection) throws SQLException {
        execute(connection, "CREATE TEMP TABLE jdbc_batch (id bigserial PRIMARY KEY, val int)");
        try (PreparedStatement insert = connection.prepareStatement(
                "INSERT INTO jdbc_batch(val) VALUES (?)", Statement.RETURN_GENERATED_KEYS)) {
            for (int i = 1; i <= 16; i++) {
                insert.setInt(1, i);
                insert.addBatch();
            }
            int[] counts = insert.executeBatch();
            require(counts.length == 16, "batch count length");
            for (int count : counts) require(count == 1 || count == Statement.SUCCESS_NO_INFO,
                    "batch member failed");
            try (ResultSet keys = insert.getGeneratedKeys()) {
                for (int i = 1; i <= 16; i++) {
                    require(keys.next() && keys.getLong(1) == i, "generated key changed");
                }
                require(!keys.next(), "extra generated key");
            }
        }
        require(scalar(connection, "SELECT sum(val) FROM jdbc_batch") == 136, "batch values changed");
        pass("batch_and_generated_keys");
    }

    private static void cursor(Connection connection) throws SQLException {
        connection.setAutoCommit(false);
        try (Statement statement = connection.createStatement()) {
            statement.setFetchSize(17);
            try (ResultSet rows = statement.executeQuery("SELECT generate_series(1,257)")) {
                for (int i = 1; i <= 257; i++) require(rows.next() && rows.getInt(1) == i,
                        "cursor lost or reordered a row");
                require(!rows.next(), "cursor returned extra rows");
            }
        }
        connection.commit();
        connection.setAutoCommit(true);
        pass("cursor_fetch");
    }

    private static void copy(Connection connection) throws Exception {
        execute(connection, "CREATE TEMP TABLE jdbc_copy(id int, val text)");
        StringBuilder input = new StringBuilder();
        for (int i = 0; i < 100; i++) input.append(i).append(",\u03bb-").append(i).append('\n');
        require(connection.unwrap(PGConnection.class).getCopyAPI().copyIn(
                "COPY jdbc_copy FROM STDIN WITH (FORMAT csv)", new StringReader(input.toString())) == 100,
                "COPY IN count");
        StringWriter output = new StringWriter();
        require(connection.unwrap(PGConnection.class).getCopyAPI().copyOut(
                "COPY (SELECT * FROM jdbc_copy ORDER BY id) TO STDOUT WITH (FORMAT csv)", output) == 100,
                "COPY OUT count");
        require(input.toString().equals(output.toString()), "COPY contents changed");
        pass("copy_roundtrip");
    }

    private static void notify(Connection connection) throws Exception {
        execute(connection, "LISTEN jdbc_notice");
        try (Connection sender = connect("extended")) {
            execute(sender, "NOTIFY jdbc_notice, 'driver-\u03bb'");
        }
        PGNotification[] notifications = connection.unwrap(PGConnection.class).getNotifications(3000);
        require(notifications != null && notifications.length == 1, "notification absent or duplicated");
        require("jdbc_notice".equals(notifications[0].getName())
                && "driver-\u03bb".equals(notifications[0].getParameter()), "notification changed");
        execute(connection, "UNLISTEN jdbc_notice");
        pass("listen_notify");
    }

    private static void timeout(Connection connection) throws SQLException {
        try (Statement statement = connection.createStatement()) {
            statement.setQueryTimeout(1);
            try {
                statement.execute("SELECT pg_sleep(30)");
                throw new IllegalStateException("query timeout did not cancel");
            } catch (SQLException error) {
                require("57014".equals(error.getSQLState()), "timeout SQLSTATE changed");
            }
        }
        require(scalar(connection, "SELECT 41+1") == 42, "timeout broke the connection");
        pass("statement_timeout_recovery");
    }

    private static void cancel(Connection observer) throws Exception {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try (Connection target = connect("extended"); Statement statement = target.createStatement()) {
            long pid = scalar(target, "SELECT pg_backend_pid()");
            Future<String> result = executor.submit(() -> {
                try {
                    statement.execute("SELECT pg_sleep(30)");
                    return "not-canceled";
                } catch (SQLException error) {
                    return error.getSQLState();
                }
            });
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            boolean waiting = false;
            while (System.nanoTime() < deadline) {
                if (scalar(observer, "SELECT count(*) FROM pg_stat_activity WHERE pid=" + pid
                        + " AND wait_event='PgSleep'") == 1) {
                    waiting = true;
                    break;
                }
                Thread.sleep(20);
            }
            require(waiting, "cancel target did not enter its observable wait");
            statement.cancel();
            require("57014".equals(result.get(5, TimeUnit.SECONDS)), "CancelRequest failed");
            require(scalar(target, "SELECT 42") == 42, "canceled connection cannot recover");
            require(scalar(observer, "SELECT 43") == 43, "cancel affected another connection");
        } finally {
            executor.shutdownNow();
            require(executor.awaitTermination(20, TimeUnit.SECONDS), "cancel executor did not terminate");
        }
        pass("cancel_request_isolation");
    }

    private static void concurrentSessions() throws Exception {
        ExecutorService executor = Executors.newFixedThreadPool(4);
        CountDownLatch ready = new CountDownLatch(4);
        CountDownLatch start = new CountDownLatch(1);
        List<Future<Long>> results = new ArrayList<>();
        try {
            for (int i = 0; i < 4; i++) {
                final int marker = i;
                results.add(executor.submit(() -> {
                    try (Connection connection = connect("extended")) {
                        execute(connection, "CREATE TEMP TABLE jdbc_local(val int)");
                        execute(connection, "INSERT INTO jdbc_local VALUES (" + marker + ")");
                        execute(connection, "SET postgamma.driver_marker='" + marker + "'");
                        ready.countDown();
                        require(start.await(10, TimeUnit.SECONDS), "session start barrier timed out");
                        long pid = scalar(connection, "SELECT pg_backend_pid()");
                        for (int n = 0; n < 50; n++) {
                            require(scalar(connection, "SELECT val FROM jdbc_local") == marker,
                                    "temp table crossed sessions");
                            require(scalar(connection, "SELECT current_setting('postgamma.driver_marker')::int")
                                    == marker, "GUC crossed sessions");
                        }
                        return pid;
                    }
                }));
            }
            require(ready.await(10, TimeUnit.SECONDS), "sessions could not connect concurrently");
            start.countDown();
            Set<Long> pids = new HashSet<>();
            for (Future<Long> result : results) require(pids.add(result.get(15, TimeUnit.SECONDS)),
                    "concurrent sessions shared a backend identifier");
        } finally {
            start.countDown();
            executor.shutdownNow();
            require(executor.awaitTermination(20, TimeUnit.SECONDS), "session executor did not terminate");
        }
        pass("concurrent_session_isolation");
    }

    public static void main(String[] arguments) throws Exception {
        require(arguments.length == 3, "expected JDBC URL, user, password");
        url = arguments[0];
        user = arguments[1];
        password = arguments[2];
        try (Connection connection = connect("extended")) {
            require(connection.isValid(2), "connection invalid");
            DatabaseMetaData metadata = connection.getMetaData();
            require("PostgreSQL".equals(metadata.getDatabaseProductName()), "unexpected wire identity");
            System.out.println("JDBC_VERSION " + metadata.getDriverVersion());
            pass("connection_metadata");
            typedResults(connection);
            transactions(connection);
            batch(connection);
            cursor(connection);
            copy(connection);
            notify(connection);
            timeout(connection);
            cancel(connection);
        }
        concurrentSessions();
        try (Connection connection = connect("simple");
             PreparedStatement statement = connection.prepareStatement("SELECT ?::int, ?::text")) {
            statement.setInt(1, 42);
            statement.setString(2, "\u03bb ' simple");
            try (ResultSet row = statement.executeQuery()) {
                require(row.next() && row.getInt(1) == 42 && "\u03bb ' simple".equals(row.getString(2)),
                        "simple protocol parameters changed");
                require(!row.next(), "simple protocol extra row");
            }
        }
        pass("simple_query_mode");
        String saved = password;
        password = "incorrect-test-password";
        try (Connection unexpected = connect("extended")) {
            require(!unexpected.isValid(1), "incorrect password accepted");
            throw new IllegalStateException("incorrect password accepted");
        } catch (SQLException error) {
            require("28P01".equals(error.getSQLState()), "authentication SQLSTATE changed");
        } finally {
            password = saved;
        }
        pass("bad_password");
        require(passed == 12, "incomplete JDBC suite");
        System.out.println("JDBC_PASS " + passed);
    }
}
