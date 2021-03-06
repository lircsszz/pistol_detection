//
//  main.cpp
//  PistolDetection
//
//  Created by John Doherty on 2/10/14.
//  Copyright (c) 2014 John Doherty, Aaron Damashek. All rights reserved.
//

#include "opencv2/contrib/contrib.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "../dlib-18.6/dlib/svm.h"
#include <iostream>
#include <fstream>
#include <numeric>
#include <ctime>
#include <random>
#include <math.h>

using namespace std;
using namespace cv;
using namespace dlib;

Vector<Vector<int>> truth;
const int order = 4;
const int numPolygons = pow(2,order);
//const int features = 2*numPolygons;
const int features = 2*16;
//const int features = 1;
bool fullML = false;

static int numFound = 0;
string folder = "./basicFinds/Found";
//string folder = "./votingFinds2/" + std::to_string(numPolygons) + "/Found";
//string folder = "./mLFinds/Found" + std::to_string(order);

typedef matrix<double, features, 1> subImageResults;
typedef radial_basis_kernel<subImageResults> kernel;
typedef decision_function<kernel> dec_funct_type;
typedef normalized_function<dec_funct_type> funct;

typedef struct {
    Mat *tpl;
    Mat *edges;
    std::vector<std::vector<Point> > *results;
    std::vector<float> *costs;
    int *best;
} thread_data;

struct image_truth{
    Vector<Mat> Images;
    Vector<int> imageTruths;
};

struct chamferResult{
    bool found;
    double cost;
};

struct subdividedResults{
    std::vector<std::vector<double> > results;
    std::vector<int> imageTruth;
};

#define MAX_HEIGHT 400
#define MAX_WIDTH 400
#define TEMPL_SCALE 1
#define MAX_MATCHES 20
#define MIN_MATCH_DIST 1.0
#define PAD_X 3
#define PAD_Y 3
#define SCALES 6
#define MIN_SCALE .6
#define MAX_SCALE 1.2
#define ORIENTATION_WEIGHT 0.0
#define TRUNCATE 20


/*Runs chamfer matching with specified parameters. Designed to be run on separate thread*/
void* runMatching(void *threadData) {
    
    thread_data *input = (thread_data *) threadData;
    *input->best = chamerMatching(*input->edges, *input->tpl, *input->results, *input->costs, TEMPL_SCALE, MAX_MATCHES, MIN_MATCH_DIST, PAD_X, PAD_Y, SCALES, MIN_SCALE, MAX_SCALE, ORIENTATION_WEIGHT, TRUNCATE);
    pthread_exit(NULL);
}

/*Spawn a new thread to run matching using the given edge image and template*/
void threadedMatching(Mat* edges, Mat* tpl, std::vector<std::vector<Point> > *results, std::vector<float> *costs, int *best, pthread_t *thread) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    size_t size;
    pthread_attr_getstacksize(&attr, &size);
    pthread_attr_setstacksize(&attr, 8*size);
    thread_data *data;
    data = (thread_data *)malloc(sizeof(thread_data));
    data->tpl = tpl;
    data->edges = edges;
    data->results = results;
    data->costs = costs;
    data->best = best;

    int rc = pthread_create(thread, &attr, runMatching, data);
    
    if(rc) {
        cout << "Error:unable to create thread," << endl;
    }
}

/*Helper function to produce image output to judge detection*/
void colorPointsInImage(Mat img, std::vector<Point>&results, Vec3b color) {
    size_t i, n = results.size();
    for( i = 0; i < n; i++ ) {
        Point pt = results[i];
        if(pt.inside(Rect(0, 0, img.cols, img.rows))) {
            img.at<Vec3b>(pt) = color;
        }
    }
}

/*Displays results in new window. Must be called from main thread*/
void displayResults(Mat img, int best, std::vector<std::vector<Point> >&results, bool showAllMatches) {
    
    if (showAllMatches) {
        size_t m = results.size();
        for(size_t j = 0; j < m; j++) {
            colorPointsInImage(img, results[j], Vec3b(0, 255, 0));
        }
    }
    colorPointsInImage(img, results[best], Vec3b(255, 0, 0));
    
    imshow("result", img);
    waitKey();
    destroyAllWindows();
}

/*Read in true values of whether or not a gun is in a specified image*/
void populateTruth(){
    //Auto-file read in
    ifstream input( "./truth.txt" );
    std::string line;
    int guns = 0;
    int non_guns = 0;
    while (std::getline(input, line)){

        Vector<int> currFolder;
        for(int num = 0; num < line.length(); num++){
            char thisNum =line[num];
            int currNum = atoi(&thisNum);
            
            if(currNum == 1){
                guns++;
            }else{
                non_guns++;
            }
            currFolder.push_back(currNum);
        }
        truth.push_back(currFolder);
    }
    cout << "Guns: " << guns << endl;
    cout << "Non-guns: " << non_guns << endl;
}

/*Read in images and store the ground truth of whether a gun is associated with the image*/
image_truth readInImages(){
    image_truth images;
    for(int i = 1; i <= 120; i++){
        if(i == 97) continue; //Ignore this folder of images
        int imgNum = 1;
        while(true){
            string folder = to_string(i);
            string pic = to_string(imgNum);
            if(i < 100) folder = "0" + folder;
            if(i < 10) folder = "0" + folder;
            if(imgNum < 10) pic = "0" + pic;
            string fileLocation = "../../images/X" + folder + "/X" + folder + "_" + pic + ".png";
            Mat img = imread(fileLocation, CV_LOAD_IMAGE_GRAYSCALE);
            Mat cimg;
            cvtColor(img, cimg, CV_GRAY2BGR);
            if(!img.data) break;
            images.Images.push_back(img);
            images.imageTruths.push_back(truth[i][imgNum]);
            imgNum++;
        }
    }
    return images;
}

/*Return whether or not a gun was identified using basic chamfer*/
chamferResult basicChamfer(Mat img, Mat tpl, bool tryFlip = true){
    chamferResult result;
    
    // Create flipped template
    Mat tpl_flip, edges, cimgFinal, resized;
    flip(tpl, tpl_flip, 1);
    
    // Resize image
    Size size = img.size();
    float ratio = (float)size.height / size.width;
    if (size.width >= size.height && size.width > MAX_WIDTH) {
        size.width = MAX_WIDTH;
        size.height = ratio * size.width;
    } else if(size.height >= size.width && size.height > MAX_HEIGHT) {
        size.height = MAX_HEIGHT;
        size.width = (1 / ratio) * size.height;
    }
    resize(img, resized, size);
    cvtColor(resized, cimgFinal, CV_GRAY2BGR);
    
    Canny(resized, edges, 70, 300, 3);
    Canny(tpl, tpl, 150, 500, 3);
    Canny(tpl_flip, tpl_flip, 150, 500, 3);
    
    std::vector<std::vector<Point>> originalResults;
    std::vector<float> originalCosts;
    std::vector<std::vector<Point>> flippedResults;
    std::vector<float> flippedCosts;
    std::vector<Point> bestMatch;
    int originalBest, flippedBest;
    void *status1, *status2;
    float bestCost;
    
    pthread_t originalMatching, flippedMatching;
    
    threadedMatching(&edges, &tpl, &originalResults, &originalCosts, &originalBest, &originalMatching);
    threadedMatching(&edges, &tpl_flip, &flippedResults, &flippedCosts, &flippedBest, &flippedMatching);
    int rc = pthread_join(originalMatching, &status1);
    if (rc) {
        cout << "Error:unable to join," << rc << endl;
        exit(-1);
    }
    
    rc = pthread_join(flippedMatching, &status2);
    if (rc) {
        cout << "Error:unable to join," << rc << endl;
        exit(-1);
    }
    
    if(originalBest==-1 && flippedBest==-1){
        result.found=false;
        result.cost = 10000;//Essentially inf
        return result;
    }else if(originalBest==-1){
        bestMatch = flippedResults[originalBest];
        bestCost = flippedCosts[originalBest];
    }else if(flippedBest==-1){
        bestMatch = originalResults[originalBest];
        bestCost = originalCosts[originalBest];
    }else if (originalCosts[originalBest] <= flippedCosts[flippedBest]) {
        bestMatch = originalResults[originalBest];
        bestCost = originalCosts[originalBest];
    } else {
        bestMatch = flippedResults[originalBest];
        bestCost = flippedCosts[originalBest];
    }
    
    result.found = true;
    result.cost = bestCost;
    
    colorPointsInImage(cimgFinal, bestMatch, Vec3b(0, 255, 0));
    imwrite( folder + std::to_string(numFound)+ ".jpg", cimgFinal );
    //imshow("test", cimgFinal);
    //waitKey();
    //destroyAllWindows();
    numFound++;
    return result;
}

/*Split image into grid of subimages*/
Vector<Mat> splitIntoImages(Mat img, int rows = 4, int cols = 4){
    Vector<Mat> subImages;
    int rowSize = ceil((float)img.rows / rows);
    int colSize = ceil((float)img.cols / cols);
    Mat subImage;
    for (int r = 0; r < rows; r ++) {
        for (int c = 0; c < cols; c ++) {
            subImage =img(Range((r*rowSize), min(((r+1) * rowSize), img.rows)),
                          Range((c*colSize), min(((c+1) * colSize), img.cols))).clone();
            if(sum(subImage)[0] != (subImage.rows * subImage.cols * 255)) {
                subImages.push_back(subImage);
            }
        }
    }
    return subImages;
}

/*Return whether or not a gun is identified based on identification of a majority of subimages*/
chamferResult votingChamfer(Mat img, Mat tpl){
    chamferResult result;
	Vector<Mat> subPolygons = splitIntoImages(tpl, order, order);
	int detected = 0;
	for(int i = 0; i < subPolygons.size(); i++){
        chamferResult subresult = basicChamfer(img, subPolygons[i].clone());
		if(subresult.found==true) detected += 1;
	}
	if(detected > numPolygons/2){
        result.found = true;
    }else{
        result.found = false;
    }
    return result;
}

subdividedResults getAllSubImageResults(Mat tpl){
    subdividedResults subResults;
    
    Vector<Mat> subPolygons = splitIntoImages(tpl, order, order);
    
    bool samplesTested = true;
    if(!samplesTested){
        ofstream results;
        results.open ("./subImageResults-" + std::to_string(order) + ".txt");
        
        ofstream truths;
        truths.open ("./subImageTruths-" + std::to_string(order) + ".txt");
        
        ofstream times;
        times.open ("./votingTimes-" + std::to_string(order) + ".txt");
        ofstream costs;
        costs.open ("./votingCosts-" + std::to_string(order) + ".txt");
        
        for(int i = 1; i <= 120; i++){
            if(i == 97) continue; //Ignore this folder of images - they are too small and too many
            int imgNum = 1;
            while(true){
                string folder = to_string(i);
                string pic = to_string(imgNum);
                if(i < 100) folder = "0" + folder;
                if(i < 10) folder = "0" + folder;
                if(imgNum < 10) pic = "0" + pic;
                string fileLocation = "../../images/X" + folder + "/X" + folder + "_" + pic + ".png";
                cout << fileLocation << endl;
                Mat img = imread(fileLocation, CV_LOAD_IMAGE_GRAYSCALE);
                Mat cimg;
                cvtColor(img, cimg, CV_GRAY2BGR);
                if(!img.data) break;
                
                std::vector<double> found;
                
                clock_t begin = clock();
                
                for(int i = 0; i < subPolygons.size(); i++){
                    chamferResult subresult = basicChamfer(img, subPolygons[i].clone());
                    if(subresult.found){
                        found.push_back(1);//1 is found
                        found.push_back(subresult.cost);
                    }else{
                        found.push_back(0);//0 is not found
                        found.push_back(subresult.cost);
                    }
                    costs << to_string(subresult.cost) << endl;
                }
                subResults.results.push_back(found);
                subResults.imageTruth.push_back(truth[i][imgNum]);
                
                clock_t end = clock();
                double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
                times << to_string(elapsed_secs) << endl;
                costs << endl;
                
                //Write samples to file
                for(int k = 0; k < found.size(); k++){
                    results << found[k] << " ";
                }
                results << endl;
                
                //Write Truths to file
                truths << std::to_string(truth[i][imgNum]) << endl;

                imgNum++;
            }
        }
        
        results.close();
    }else{
        //read samples from file
        //How would this work with costs as well??????
        ifstream input( "./subImageResults-" + std::to_string(order) + ".txt" );
        std::string line;
        while (std::getline(input, line)){
            std::vector<double> currImage;
            
            std::stringstream ss(line);
            std::istream_iterator<std::string> begin(ss);
            std::istream_iterator<std::string> end;
            std::vector<std::string> vstrings(begin, end);
            
            for(int num = 0; num < vstrings.size(); num++){
                double currNum = atof(vstrings[num].c_str());
                //if(currNum == 10000) currNum = 1.5;
                currImage.push_back(currNum);
            }
            subResults.results.push_back(currImage);
        }
        
        ifstream input2( "./subImageTruths-" + std::to_string(order) + ".txt" );
        std::string line2;
        while (std::getline(input2, line2)){
            int truth = atoi(line2.c_str());
            if(truth != 1) truth = -1;
            subResults.imageTruth.push_back(truth);
        }
    }
    return subResults;
}

std::vector<std::vector<double> > getAllFullImageCosts(){
    std::vector<std::vector<double> > costs;

    ifstream input( "./basicCosts.txt" );
    std::string line;
    while (std::getline(input, line)){
        std::vector<double> cost;
        double currCost = atof(line.c_str());
        if(currCost == 10000) currCost = 0.5;
        cost.push_back(currCost);
        costs.push_back(cost);
    }
    return costs;
}

/*Calculate the function to be used for ML using training samples*/
funct setUpMLChamfer(Mat tpl, std::vector<std::vector<double> > imageFeatures, std::vector<int> imageTruths){

    std::vector<subImageResults> samples;
    std::vector<double> labels;//Ground truth
    
    for(int i = 0; i < imageFeatures.size(); i++){
        subImageResults sample;
        for(int j = 0; j < features; j++){
            sample(j) = imageFeatures[i][j];
        }
        samples.push_back(sample);
        labels.push_back(imageTruths[i]);
    }
    
    //Normalize samples
    vector_normalizer<subImageResults> normalizer;
    normalizer.train(samples);
    for (unsigned long i = 0; i < samples.size(); i++){
        //samples[i] = normalizer(samples[i]);
    }
    
    //randomize_samples(samples, labels);
    
    // The nu parameter has a maximum value that is dependent on the ratio of the +1 to -1
    // labels in the training data.  This function finds that value.
    
    const double max_nu = maximum_nu(labels);
    // here we make an instance of the svm_nu_trainer object that uses our kernel type.
    svm_nu_trainer<kernel> trainer;
    cout << "doing cross validation" << endl;
    for (double gamma = 0.00001; gamma <= 1; gamma *= 5)
    {
        for (double nu = 0.00001; nu < max_nu; nu *= 5)
        {
            // tell the trainer the parameters we want to use
            trainer.set_kernel(kernel(gamma));
            trainer.set_nu(nu);
            
            cout << "gamma: " << gamma << "    nu: " << nu << endl;
            // Print out the cross validation accuracy for 3-fold cross validation using
            // the current gamma and nu.  cross_validate_trainer() returns a row vector.
            // The first element of the vector is the fraction of +1 training examples
            // correctly classified and the second number is the fraction of -1 training
            // examples correctly classified.
            cout << "cross validation accuracy: " << cross_validate_trainer(trainer, samples, labels, 3) << endl;
        }
    }
    
    trainer.set_kernel(kernel(0.78125));
    trainer.set_nu(0.15625);
    
    // Here we are making an instance of the normalized_function object.  This object
    // provides a convenient way to store the vector normalization information along with
    // the decision function we are going to learn.
    funct learned_function;
    //learned_function.normalizer = normalizer;  // save normalization information
    learned_function.function = trainer.train(samples, labels); // perform the actual SVM training and save the results
    
    // print out the number of support vectors in the resulting decision function
    cout << "\nnumber of support vectors in our learned_function is "
    << learned_function.function.basis_vectors.size() << endl;

	return learned_function;
}

/*Use trained system to determine whether or not a gun is present based on subimage results*/
bool MLChamfer(Mat img, Mat tpl, funct decisionFunction){
	Vector<Mat> subPolygons = splitIntoImages(tpl, order, order);
    
    //Find results for sub-images
    subImageResults found;
	for(int i = 0; i < subPolygons.size(); i++){
        chamferResult subresult = basicChamfer(img, subPolygons[i].clone());
        if(subresult.found){
            found(2*i) = 1;//1 is found
            found(2*i+1) = (subresult.cost);
        }else{
            found(2*i) = -1;//0 is not found
            found(2*i+1) = (subresult.cost);
        }
	}
    
    //Do Machine Learning to determine if whole image matches
    double output = decisionFunction(found);
    
    cout << "The classifier output is " << output << endl;
    
    //The decision function will return values
    // >= 0 for samples it predicts are in the +1 class and numbers < 0 for samples it
    // predicts to be in the -1 class.
	return (output >= 0);
}

/*Use trained system to determine whether or not a gun is present based on subimage results*/
bool MLChamferByFeatures(std::vector<double> features, funct decisionFunction){
    //Find results for sub-images
    subImageResults found;
	for(int i = 0; i < features.size(); i++){
        found(i) = features[i];
	}
    
    //Do Machine Learning to determine if whole image matches
    double output = decisionFunction(found);
    
    cout << "The classifier output is " << output << endl;
    
    //The decision function will return values
    // >= 0 for samples it predicts are in the +1 class and numbers < 0 for samples it
    // predicts to be in the -1 class.
	return (output >= 0);
}

/*Report the results of a test*/
void reportResults(int falsePositives, int falseNegatives, int correctIdentification, int correctDiscard){
    ofstream results;
    results.open (folder + "-Results.txt");
    
    int sum = falsePositives + falseNegatives + correctDiscard + correctIdentification;
    results << "False Positives: " << falsePositives << endl;
    results << "False Negatives: " << falseNegatives << endl;
    results << "Correct Identifications: " << correctIdentification << endl;
    results << "Correct Discards: " << correctDiscard << endl;
    
    cout << "False Positives: " << falsePositives << endl;
    cout << "False Negatives: " << falseNegatives << endl;
    cout << "Correct Identifications: " << correctIdentification << endl;
    cout << "Correct Discards: " << correctDiscard << endl;
    
    if(correctIdentification > 0){
        double precision = (double) correctIdentification/(correctIdentification + falsePositives);
        double recall = (double) correctIdentification/(correctIdentification + falseNegatives);
        double F1 = 2*precision*recall/(precision+recall);
        results << "Precision: " << precision << endl;
        results << "Recall: " << recall << endl;
        results << "F1 Score: " << F1 << endl;
        
        cout << "Precision: " << precision << endl;
        cout << "Recall: " << recall << endl;
        cout << "F1 Score: " << F1 << endl;
    }
    results << "Success rate: " << (double)(correctDiscard + correctIdentification)/sum*100 << endl;
    cout << "Success rate: " << (double)(correctDiscard + correctIdentification)/sum*100 << endl;
}

/*Test the performance of basic chamfer against all images*/
void basicChamferTest(Mat tpl){//Basic or votingChamfer
    
    ofstream times;
    times.open ("./basicTimes.txt");
    ofstream costs;
    costs.open ("./basicCosts.txt");
    
    int falsePositives = 0;
    int falseNegatives = 0;
    int correctIdentification = 0;
    int correctDiscard = 0;
    
    for(int i = 62; i <= 120; i++){
        if(i == 97) continue; //Ignore this folder of images - they are too small and too many
        int imgNum = 1;
        while(true){//Go through folder
            string folder = to_string(i);
            string pic = to_string(imgNum);
            if(i < 100) folder = "0" + folder;
            if(i < 10) folder = "0" + folder;
            if(imgNum < 10) pic = "0" + pic;
            string fileLocation = "../../images/X" + folder + "/X" + folder + "_" + pic + ".png";
            cout << fileLocation << endl;
            Mat img = imread(fileLocation, CV_LOAD_IMAGE_GRAYSCALE);
            /*imshow("test", img);
            waitKey();
            destroyAllWindows();*/
            img.ptr();
            Mat cimg;
            
            cvtColor(img, cimg, CV_GRAY2BGR);
            if(!img.data) break;
            bool imgTruth = truth[i][imgNum];
            clock_t begin = clock();

            chamferResult test = basicChamfer(img, tpl.clone());
            bool gunFound = test.found;
            
            clock_t end = clock();
            double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
            times << to_string(elapsed_secs) << endl;
            costs << to_string(test.cost) << endl;
            
            
            if(gunFound){
                if(imgTruth){
                    correctIdentification+=1;
                }else{
                    falsePositives+=1;
                }
            }
            if(!gunFound){
                if(!imgTruth){
                    correctDiscard+=1;
                }else{
                    falseNegatives+=1;
                }
            }
            
            imgNum++;
        }
    }
    reportResults(falsePositives, falseNegatives, correctIdentification, correctDiscard);
}

/*Test the performance of voting chamfer against all images*/
void votingChamferTest(Mat tpl){
    int falsePositives = 0;
    int falseNegatives = 0;
    int correctIdentification = 0;
    int correctDiscard = 0;
    subdividedResults subResults = getAllSubImageResults(tpl);
    
    for(int i = 0; i < subResults.imageTruth.size(); i++){
        bool imgTruth = subResults.imageTruth[i];
        bool gunFound;
        std::vector<double> img = subResults.results[i];
        double detected = 0;
        for(int k = 0; k < img.size(); k++){
            detected += img[k];
        }
        
        if(detected > numPolygons/2){
            gunFound = true;
        }else{
            gunFound = false;
        }
        
        if(gunFound){
            if(imgTruth){
                correctIdentification+=1;
            }else{
                falsePositives+=1;
            }
        }
        if(!gunFound){
            if(!imgTruth){
                correctDiscard+=1;
            }else{
                falseNegatives+=1;
            }
        }
    }
    reportResults(falsePositives, falseNegatives, correctIdentification, correctDiscard);
}


/*Test the performance of ML chamfer against all images*/
void mlChamferTest(Mat tpl){

    int falsePositives = 0;
    int falseNegatives = 0;
    int correctIdentification = 0;
    int correctDiscard = 0;
    
    ofstream times;
    times.open ("./mLTimes.txt");

    subdividedResults subResults = getAllSubImageResults(tpl);
    std::vector<std::vector<double> > features = subResults.results;//costs
    std::vector<int> truth = subResults.imageTruth;//Gun truths
    
    if(fullML){
        features = getAllFullImageCosts();
    }
    
    srand (time(NULL));
    int seed = std::rand();
    cout << "Seed: " << seed << endl;
    std::srand ( seed );
    std::random_shuffle ( features.begin(), features.end() );
    
    std::srand ( seed );
    std::random_shuffle ( truth.begin(), truth.end() );
    
    //randomize_samples(subResults.results, subResults.imageTruth);//Make sure to test that it actually randomizes them
    
    double splitProportion = (double)7/10;
    std::size_t const splitSize = features.size()*splitProportion; //Make sure to test and see that this works
    std::vector<std::vector<double> > trainingImages(features.begin(), features.begin() + splitSize);
    std::vector<std::vector<double> > testImages(features.begin() + splitSize, features.end());
    std::vector<int> trainingTruths(truth.begin(), truth.begin() + splitSize);
    std::vector<int> testTruths(truth.begin() + splitSize, truth.end());
    
    cout << "Training " << std::accumulate(trainingTruths.begin(),trainingTruths.end(),0) << endl;
    cout << "Testing " << std::accumulate(testTruths.begin(),testTruths.end(),0) << endl;
    
    //Set-up ML
    funct decision = setUpMLChamfer(tpl, trainingImages, trainingTruths);
    
    //Run ML
    for(int i = 0; i < testImages.size(); i++){
        std::vector<double> features = testImages[i];
        
        clock_t begin = clock();
        
        bool gunFound = MLChamferByFeatures(features, decision); //MLChamfer
        
        clock_t end = clock();
        double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
        times << to_string(elapsed_secs) << endl;
        
        if(gunFound){
            if(testTruths[i]>0){
                correctIdentification+=1;
            }else{
                falsePositives+=1;
            }
        }
        if(!gunFound){
            if(!(testTruths[i] > 0)){
                correctDiscard+=1;
            }else{
                falseNegatives+=1;
            }
        }
    }
    reportResults(falsePositives, falseNegatives, correctIdentification, correctDiscard);
}

int main( int argc, char** argv ) {
    Mat tpl;
    tpl = imread("./pistol_tpl.jpg", CV_LOAD_IMAGE_GRAYSCALE);
    
    populateTruth();
    basicChamferTest(tpl);
    //votingChamferTest(tpl);
    //mlChamferTest(tpl);
    return 0;
}




