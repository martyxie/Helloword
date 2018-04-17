
public class testjni{

	public native void print();
	public static void main(String args[]){
		System.loadLibrary("testjni");
		testjni.print();
	}
}
