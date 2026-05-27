package dev.cajeta.idea.wizard

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption

/**
 * Materializes the on-disk file layout for a freshly-created
 * Cajeta project. Kept separate from the wizard step so the same
 * scaffolder can be exercised by unit tests without IntelliJ
 * platform classes.
 */
object CajetaProjectScaffold {

    data class Params(
        val projectRoot: Path,
        val projectName: String,
        val packageName: String,
        val entryClass: String,
    )

    fun materialize(params: Params) {
        val src = params.projectRoot.resolve("src")
            .resolve(params.packageName.replace('.', '/'))
        Files.createDirectories(src)

        writeIfAbsent(params.projectRoot.resolve("cajeta.json"), manifest(params))
        writeIfAbsent(src.resolve("${params.entryClass}.cajeta"), appSource(params))
        writeIfAbsent(src.resolve("Point.cajeta"), pointSource(params))
        writeIfAbsent(params.projectRoot.resolve(".gitignore"), gitignore())
    }

    private fun writeIfAbsent(path: Path, content: String) {
        if (Files.exists(path)) return
        Files.createDirectories(path.parent)
        Files.write(
            path,
            content.toByteArray(StandardCharsets.UTF_8),
            StandardOpenOption.CREATE_NEW,
            StandardOpenOption.WRITE,
        )
    }

    private fun manifest(p: Params): String = """
        {
          "package": "${p.packageName}",
          "name": "${p.projectName}",
          "version": "0.1.0",
          "entry": "${p.packageName}.${p.entryClass}.main",
          "dependencies": {}
        }
    """.trimIndent() + "\n"

    private fun appSource(p: Params): String = """
        package ${p.packageName};

        import cajeta.collection.ArrayList;

        // # ${p.projectName}
        //
        // Welcome to Cajeta. Things to try:
        //   - **Stack vs heap** allocation is explicit at every `new`-less site.
        //   - The `#` prefix transfers ownership into a collection.
        //   - `stream().map(...).reduce(...)` mirrors Java's streams.
        //
        // Run with `cajeta run` (see cajeta-docs/BuildTool.md).
        public final class ${p.entryClass} {

            public static int32 main() {
                Point a = stack Point(3, 4);          // dropped on scope exit
                Point b = heap  Point(5, 12);         // freed by drop chain

                ArrayList<Point> points = heap ArrayList<Point>();
                points.add(#b);                       // ownership of b transfers in

                int32 stackSquared = a.distSq();      // 25
                int32 sumSquared = points.stream()
                    .map<int32>((p) -> p.distSq())
                    .reduce(0, (acc, v) -> acc + v);  // 169

                System.stdout.println("a.distSq() = " + stackSquared);
                System.stdout.println("sum over points = " + sumSquared);

                return 0;
            }
        }
    """.trimIndent() + "\n"

    private fun pointSource(p: Params): String = """
        package ${p.packageName};

        // A trivial value type. Both fields are unboxed int32, so a
        // `stack Point(...)` allocation stays on the current frame
        // without indirection.
        public class Point {
            public int32 x;
            public int32 y;

            public Point(int32 x, int32 y) {
                this.x = x;
                this.y = y;
            }

            public int32 distSq() {
                return this.x * this.x + this.y * this.y;
            }
        }
    """.trimIndent() + "\n"

    private fun gitignore(): String = """
        # Cajeta build outputs
        build/
        target/
        *.cja
        *.uber

        # IDE
        .idea/
        *.iml
        .vscode/
    """.trimIndent() + "\n"
}
