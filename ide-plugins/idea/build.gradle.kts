import org.jetbrains.intellij.platform.gradle.TestFrameworkType

plugins {
    id("org.jetbrains.kotlin.jvm")
    id("org.jetbrains.intellij.platform")
    id("antlr")
}

group = providers.gradleProperty("pluginGroup").get()
version = providers.gradleProperty("pluginVersion").get()

kotlin {
    jvmToolchain(providers.gradleProperty("javaVersion").get().toInt())
}

// Grammar lives in the cajeta repo at the project root, two levels up.
val grammarDir = file("../../antlr4")

dependencies {
    // ANTLR4 codegen + runtime
    antlr("org.antlr:antlr4:4.13.2")
    implementation("org.antlr:antlr4-runtime:4.13.2")

    // Bridge ANTLR4 ParseTree to IntelliJ PSI.
    implementation("org.antlr:antlr4-intellij-adaptor:0.1")

    // Markdown engine for Obsidian-style comment rendering (M5).
    implementation("org.jetbrains:markdown:0.7.3")

    testImplementation("junit:junit:4.13.2")

    intellijPlatform {
        create(
            providers.gradleProperty("platformType"),
            providers.gradleProperty("platformVersion"),
        )
        testFramework(TestFrameworkType.Platform)
    }
}

intellijPlatform {
    pluginConfiguration {
        name = providers.gradleProperty("pluginName")
        version = providers.gradleProperty("pluginVersion")

        ideaVersion {
            sinceBuild = providers.gradleProperty("pluginSinceBuild")
            untilBuild = providers.gradleProperty("pluginUntilBuild")
        }
    }

    // Marketplace signing + publishing (plan §8). All inputs come from
    // environment variables (CI secrets), so configure is a no-op when they're
    // unset — `buildPlugin`/`verifyPlugin` don't need them, and `signPlugin`/
    // `publishPlugin` only run on production tags in release.yml.
    signing {
        certificateChain = providers.environmentVariable("CERTIFICATE_CHAIN")
        privateKey = providers.environmentVariable("PRIVATE_KEY")
        password = providers.environmentVariable("PRIVATE_KEY_PASSWORD")
    }

    publishing {
        token = providers.environmentVariable("PUBLISH_TOKEN")
        // Stable channel by default; release.yml skips publishing pre-release
        // tags entirely (so EAP-channel routing isn't needed yet).
        channels = providers.gradleProperty("pluginChannel")
            .map { listOf(it) }
            .orElse(listOf("default"))
    }

    // Plugin Verifier targets. A locally installed IDE (e.g. CLion 261, the real
    // install target here) is used when CAJETA_VERIFY_IDE points at it, so the
    // verifier runs against the exact IDE with no multi-GB download; otherwise
    // it falls back to JetBrains' recommended IDE set.
    pluginVerification {
        ides {
            val localIde = providers.environmentVariable("CAJETA_VERIFY_IDE").orNull
            if (localIde != null) local(file(localIde)) else recommended()
        }
    }
}

// The org.gradle.antlr plugin resolves source paths relative to the
// source set's antlr/ directory. Stage the canonical grammar files
// from the cajeta repo into src/main/antlr/ before codegen runs.
val stageGrammar by tasks.registering(Copy::class) {
    from(grammarDir) { include("*.g4") }
    into(layout.projectDirectory.dir("src/main/antlr"))
}

tasks.generateGrammarSource {
    dependsOn(stageGrammar)
    arguments = arguments + listOf(
        "-package", "dev.cajeta.idea.parser.antlr",
        "-visitor",
        "-long-messages",
    )
    outputDirectory = file("build/generated-src/antlr/main/dev/cajeta/idea/parser/antlr")
}

// Staged grammar files are build outputs of the cajeta repo; never commit them.
tasks.clean {
    delete(layout.projectDirectory.dir("src/main/antlr"))
}

// Scans CajetaLexer.g4 and emits a Kotlin source listing every
// token whose literal is an alphabetic word — that's both keywords
// (class, public, …) and primitive type names (int32, char, …).
// The syntax highlighter uses this generated set, so adding a new
// keyword in the grammar requires only a rebuild — not a hand edit
// to the highlighter.
val generateTokenCategories by tasks.registering {
    val grammarFile = grammarDir.resolve("CajetaLexer.g4")
    val outFile = layout.buildDirectory.file(
        "generated-src/keywords/dev/cajeta/idea/highlighting/CajetaKeywords.kt"
    ).get().asFile

    inputs.file(grammarFile)
    outputs.file(outFile)

    doLast {
        val tokenLineRe = Regex("""^([A-Z_][A-Z0-9_]*)\s*:\s*'([^']*)'\s*;""")
        val alphaLiteralRe = Regex("""^[A-Za-z][A-Za-z0-9_-]*$""")

        val keywords = grammarFile.readLines().mapNotNull { line ->
            val m = tokenLineRe.matchEntire(line.trim()) ?: return@mapNotNull null
            val tokenName = m.groupValues[1]
            val literal = m.groupValues[2]
            if (alphaLiteralRe.matches(literal)) tokenName else null
        }

        outFile.parentFile.mkdirs()
        outFile.writeText(buildString {
            appendLine("// Generated from antlr4/CajetaLexer.g4 by the")
            appendLine("// generateTokenCategories Gradle task. Do not edit by hand.")
            appendLine("package dev.cajeta.idea.highlighting")
            appendLine()
            appendLine("import dev.cajeta.idea.parser.antlr.CajetaLexer")
            appendLine()
            appendLine("internal object CajetaKeywords {")
            appendLine("    /** ANTLR token IDs whose literal is an alphabetic word. */")
            appendLine("    val ALL: Set<Int> = setOf(")
            for (name in keywords) {
                appendLine("        CajetaLexer.$name,")
            }
            appendLine("    )")
            appendLine("}")
        })
    }
}

tasks.named("compileKotlin") {
    dependsOn(tasks.generateGrammarSource)
    dependsOn(generateTokenCategories)
}

sourceSets {
    main {
        java.srcDir("build/generated-src/antlr/main")
        java.srcDir("build/generated-src/keywords")
    }
}
