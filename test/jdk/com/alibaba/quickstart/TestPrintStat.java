/*
 * @test
 * @summary Test -Xquickstart:printStat option
 * @library /test/lib
 * @requires os.arch=="amd64"
 * @run main/othervm/timeout=600 TestPrintStat
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestPrintStat {
    private static String cachepath = System.getProperty("user.dir");
    public static void main(String[] args) throws Exception {
        TestPrintStat test = new TestPrintStat();
        cachepath = cachepath + "/printstat";
        test.runWithoutCache();
        test.runAsTracer();
        test.runAsReplayerWithPrintStat();
    }

    void runWithoutCache() throws Exception {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xquickstart:path=" + cachepath, "-Xquickstart:printStat", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        System.out.println(output.getOutput());
        output.shouldContain("[QuickStart] There is no cache in " + cachepath);
        output.shouldHaveExitValue(0);
    }

    void runAsTracer() throws Exception {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xquickstart:path=" + cachepath, "-Xquickstart:verbose", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        System.out.println(output.getOutput());
        output.shouldContain("Running as tracer");
        output.shouldHaveExitValue(0);
    }

    void runAsReplayerWithPrintStat() throws Exception {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xquickstart:path=" + cachepath, "-Xquickstart:printStat", "-version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        System.out.println(output.getOutput());
        output.shouldContain("[QuickStart] Current statistics for cache " + cachepath);
        output.shouldHaveExitValue(0);
    }
}

